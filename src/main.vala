/*
 * ewpi.vala — EFL Windows Package Installer
 *
 * A single-file Vala rewrite of the original ewpi.c / ewpi_map.c / ewpi_spawn.c.
 *
 * Cross-compiles and installs the EFL dependency stack for MinGW targets from
 * Linux. Reads one ".ewpi" descriptor per package (name/version/url/deps),
 * resolves the dependency tree, then downloads, extracts and builds each
 * package via its own "install.sh" script.
 *
 * Build:
 *   valac --pkg glib-2.0 --pkg gio-2.0 --pkg posix -o ewpi ewpi.vala
 *
 * Notable differences from the C original (why the rewrite looks different):
 *   - ewpi_map.c's manual mmap/CreateFileMapping wrapper is replaced by
 *     GLib.MappedFile, which is already cross-platform.
 *   - ewpi_spawn.c's hand-rolled fork/exec and CreateProcess paths collapse
 *     into GLib.Subprocess, which is portable and needs no #ifdef _WIN32.
 *   - Shell command strings built with strcat() are replaced with argv
 *     arrays or direct GLib.File/FileUtils calls, so there is no manual
 *     buffer sizing and no shell-quoting to get wrong.
 *   - mkdir -p is DirUtils.create_with_parents(); the original's manual
 *     path-walking mkdir_p is no longer needed.
 */

using GLib;

// ---------------------------------------------------------------------
// Package descriptor, parsed from <name>/<name>.ewpi
// ---------------------------------------------------------------------

public class Package : Object {
    public string name;
    public string version = "";
    public int vmaj = 0;
    public int vmin = 0;
    public int vmic = 0;
    public int vrev = 0;
    public string url = "";
    public string tarname = "";
    public string[] deps = {};
    public bool is_git = false;

    public bool downloaded = false;
    public bool extracted = false;
    public bool installed = false;

    public Package(string name) {
        this.name = name;
    }

    // true if `this` is a strictly newer version than `other`
    public bool newer_than(Package other) {
        if (vmaj != other.vmaj) return vmaj > other.vmaj;
        if (vmin != other.vmin) return vmin > other.vmin;
        if (vmic != other.vmic) return vmic > other.vmic;
        return vrev > other.vrev;
    }

    // Parse "name: ", "version: ", "url: " and "deps: a b c" lines out of
    // the text of a .ewpi descriptor. Returns null (with a warning on
    // stderr identifying the offending descriptor) if a required field is
    // missing or malformed, rather than silently building a half-filled
    // Package that fails confusingly later on.
    public static Package? parse(string descriptor_path, string text) {
        var pkg = new Package("");
        bool saw_url = false;

        foreach (unowned string line in text.split("\n")) {
            if (line.has_prefix("name: ")) {
                pkg.name = line.substring(6).chomp();
            } else if (line.has_prefix("version: ")) {
                pkg.version = line.substring(9).chomp();
                if (!pkg.parse_version()) {
                    warning("%s: malformed version '%s'", descriptor_path, pkg.version);
                    return null;
                }
            } else if (line.has_prefix("url: ")) {
                pkg.url = line.substring(5).chomp();
                saw_url = true;
                int slash = pkg.url.last_index_of_char('/');
                string basename = (slash >= 0) ? pkg.url.substring(slash + 1) : pkg.url;
                pkg.tarname = basename;
                int dot = basename.last_index_of_char('.');
                if (dot >= 0 && basename.substring(dot + 1) == "git")
                    pkg.is_git = true;
            } else if (line.has_prefix("deps:")) {
                string rest = line.substring(5).strip();
                pkg.deps = (rest == "") ? new string[0] : rest.split(" ");
            }
        }

        if (pkg.name == "") {
            warning("%s: missing 'name:' field", descriptor_path);
            return null;
        }
        if (!saw_url) {
            warning("%s: missing 'url:' field", descriptor_path);
            return null;
        }

        return pkg;
    }

    // "1.2.3-4" -> vmaj=1 vmin=2 vmic=3 vrev=4. Returns false if any
    // present component isn't a plain non-negative integer.
    private bool parse_version() {
        string v = version;
        string rev_part = "";
        int dash = v.index_of_char('-');
        if (dash >= 0) {
            rev_part = v.substring(dash + 1);
            v = v.substring(0, dash);
        }

        string[] parts = v.split(".");
        if (parts.length < 1 || parts.length > 3)
            return false;

        int[] slots = { 0, 0, 0 };
        for (int i = 0; i < parts.length; i++) {
            int64 n;
            if (!int64.try_parse(parts[i], out n) || n < 0)
                return false;
            slots[i] = (int) n;
        }
        vmaj = slots[0];
        vmin = slots[1];
        vmic = slots[2];

        if (rev_part != "") {
            int64 n;
            if (!int64.try_parse(rev_part, out n) || n < 0)
                return false;
            vrev = (int) n;
        }

        return true;
    }
}

// ---------------------------------------------------------------------
// Main installer
// ---------------------------------------------------------------------

public class Ewpi : Object {
    public const int VMAJ = 1;
    public const int VMIN = 3;

    const string[] REQ_HOST_TOOLS = {
        "gcc", "g++", "ar", "dlltool", "nm", "ranlib", "strip", "windres"
    };

    struct ToolCheck {
        unowned string tool;
        unowned string version_flag;
    }

    // Every plain (non-host-prefixed) tool the pipeline invokes later:
    // make/cmake/etc. for the various packages' install.sh scripts, and
    // git/wget/tar/makensis for download_all()/extract_all()/
    // build_nsis_installer() themselves. Checking all of them up front
    // means a missing tool is reported here, not mid-build.
    const ToolCheck[] REQ_TOOLS = {
        { "make", "--version" }, { "cmake", "--version" },
        { "python", "--version" }, { "perl", "--version" },
        { "meson", "--version" }, { "ninja", "--version" },
        { "yasm", "--version" }, { "nasm", "--version" },
        { "gperf", "--version" }, { "wget", "--version" },
        { "git", "--version" }, { "tar", "--version" },
        { "bison", "--version" }, { "flex", "--version" },
        { "itstool", "--version" }, { "makensis", "-VERSION" }
    };

    string package_dir_git;
    string package_dir_dst;

    // name -> Package, plus the order packages were discovered in the git dir
    HashTable<string, Package> packages = new HashTable<string, Package>(str_hash, str_equal);
    string[] all_names = {};

    // dependency-resolved build order (topological, deps before dependents)
    string[] order = {};

    int name_field_width = 0;

    // -------------------------------------------------------------
    // Small filesystem / process helpers
    // -------------------------------------------------------------

    static bool path_is_dir(string path) {
        return FileUtils.test(path, FileTest.IS_DIR);
    }

    // Returns the file size, or 0 if it doesn't exist / isn't a regular file.
    static int64 regular_file_size(string path) {
        if (!FileUtils.test(path, FileTest.IS_REGULAR))
            return 0;
        try {
            var info = File.new_for_path(path).query_info(
                FileAttribute.STANDARD_SIZE, FileQueryInfoFlags.NONE);
            return info.get_size();
        } catch (Error e) {
            return 0;
        }
    }

    static void mark(string dir, string marker) {
        try {
            FileUtils.set_contents(Path.build_filename(dir, marker), "1\n");
        } catch (Error e) {
            warning("could not write marker %s: %s", marker, e.message);
        }
    }

    static bool has_mark(string dir, string marker) {
        return regular_file_size(Path.build_filename(dir, marker)) > 0;
    }

    // Runs argv, discarding its stdout/stderr, and returns whether it
    // exited with status 0. Equivalent to the original ewpi_spawn().
    // `error_out`, when non-null, is filled with why it failed: either the
    // GLib.Error from trying to launch it (e.g. "No such file or
    // directory" for a missing binary) or a description of the non-zero
    // exit, so callers can report something more useful than plain "no".
    static bool spawn_quiet(string[] argv, out string? error_out, string? cwd = null) {
        error_out = null;
        try {
            var launcher = new SubprocessLauncher(
                SubprocessFlags.STDOUT_SILENCE | SubprocessFlags.STDERR_SILENCE);
            if (cwd != null)
                launcher.set_cwd(cwd);
            var proc = launcher.spawnv(argv);
            proc.wait();
            if (!proc.get_successful())
                error_out = exit_reason(proc);
            return proc.get_successful();
        } catch (Error e) {
            error_out = e.message;
            return false;
        }
    }

    // Runs argv with output visible (for downloads/extraction/install
    // scripts, where the user wants to see progress), returns success.
    static bool spawn_visible(string[] argv, out string? error_out, string? cwd = null) {
        error_out = null;
        try {
            var launcher = new SubprocessLauncher(SubprocessFlags.NONE);
            if (cwd != null)
                launcher.set_cwd(cwd);
            var proc = launcher.spawnv(argv);
            proc.wait();
            if (!proc.get_successful())
                error_out = exit_reason(proc);
            return proc.get_successful();
        } catch (Error e) {
            error_out = e.message;
            return false;
        }
    }

    static string exit_reason(Subprocess proc) {
        if (proc.get_if_signaled())
            return "killed by signal %d".printf(proc.get_term_sig());
        return "exited with status %d".printf(proc.get_exit_status());
    }

    // Checks every tool the pipeline will need, printing "yes"/"no" (and,
    // on failure, why) for each — same visible format as the original,
    // but a missing tool no longer just says "no" with no explanation.
    bool check_requirements(string host) {
        bool all_ok = true;

        foreach (unowned string tool in REQ_HOST_TOOLS) {
            string? err;
            bool ok = spawn_quiet({ "%s-%s".printf(host, tool), "--version" }, out err);
            stdout.printf("  %s : %s\n", tool, ok ? "yes" : "no (%s)".printf(err));
            stdout.flush();
            all_ok &= ok;
        }

        foreach (var req in REQ_TOOLS) {
            string? err;
            bool ok = spawn_quiet({ req.tool, req.version_flag }, out err);
            stdout.printf("  %s : %s\n", req.tool, ok ? "yes" : "no (%s)".printf(err));
            stdout.flush();
            all_ok &= ok;
        }

        return all_ok;
    }

    // -------------------------------------------------------------
    // Package discovery
    // -------------------------------------------------------------

    void set_package_dirs(string prefix) {
        package_dir_git = Path.build_filename(Environment.get_current_dir(), "packages");
        package_dir_dst = Path.build_filename(prefix, "share", "ewpi", "packages");
        DirUtils.create_with_parents(package_dir_dst, 0755);
    }

    // Reads <package_dir_git>/<name>/<name>.ewpi for every subdirectory and
    // fills `packages` + `all_names`.
    bool load_packages() {
        Dir dir;
        try {
            dir = Dir.open(package_dir_git);
        } catch (Error e) {
            warning("cannot open %s: %s", package_dir_git, e.message);
            return false;
        }

        string? name;
        while ((name = dir.read_name()) != null) {
            string descriptor = Path.build_filename(package_dir_git, name, name + ".ewpi");

            string text;
            try {
                FileUtils.get_contents(descriptor, out text);
            } catch (Error e) {
                continue; // not every entry need be a package directory
            }

            var pkg = Package.parse(descriptor, text);
            if (pkg == null)
                continue; // parse() already warned about the specific problem

            if (pkg.name != name)
                warning("%s: descriptor name '%s' does not match directory name '%s'",
                        descriptor, pkg.name, name);

            packages[name] = pkg;
            all_names += name;
        }

        return true;
    }

    // Copies <path_git>/<filename> to <path_dst>/<filename>. Returns false
    // on any I/O error and, when `error_out` is non-null, fills it with the
    // reason (so callers can decide whether a missing file is fine — e.g.
    // an optional cross_toolchain.txt — or worth reporting).
    bool copy_file(string path_git, string path_dst, string filename, out string? error_out) {
        error_out = null;
        var src = File.new_for_path(Path.build_filename(path_git, filename));
        var dst = File.new_for_path(Path.build_filename(path_dst, filename));
        try {
            return src.copy(dst, FileCopyFlags.OVERWRITE);
        } catch (Error e) {
            error_out = e.message;
            return false;
        }
    }

    // For every package: create its destination directory the first time,
    // or — if it already exists — compare the cached .ewpi version there
    // against the one in git and drop the downloaded/extracted/installed
    // markers if git now has a newer version. Then (re-)copy the package's
    // build files, since the build system may have changed even when the
    // version hasn't.
    void sync_dst_dirs(string prefix) {
        foreach (unowned string name in all_names) {
            var pkg = packages[name];
            string src_dir = Path.build_filename(package_dir_git, name);
            string dst_dir = Path.build_filename(package_dir_dst, name);
            string descriptor = name + ".ewpi";

            if (path_is_dir(dst_dir)) {
                string cached_path = Path.build_filename(dst_dir, descriptor);
                try {
                    string text;
                    FileUtils.get_contents(cached_path, out text);
                    var cached = Package.parse(cached_path, text);
                    if (cached != null && pkg.newer_than(cached)) {
                        FileUtils.unlink(Path.build_filename(dst_dir, "downloaded"));
                        FileUtils.unlink(Path.build_filename(dst_dir, "extracted"));
                        FileUtils.unlink(Path.build_filename(dst_dir, "installed"));
                    }
                } catch (Error e) {
                    // no cached descriptor yet; nothing to compare against
                }
            } else {
                DirUtils.create_with_parents(dst_dir, 0755);
            }

            // The descriptor and install script are required for this
            // package to ever build; a failure to copy them is worth
            // surfacing immediately rather than discovering it much later
            // when install_all() can't find install.sh.
            string? err;
            if (!copy_file(src_dir, dst_dir, descriptor, out err))
                warning("%s: could not stage descriptor: %s", pkg.name, err);
            if (!copy_file(src_dir, dst_dir, "install.sh", out err))
                warning("%s: could not stage install.sh: %s", pkg.name, err);
            // cross_toolchain.txt is optional — not every package needs one.
            string? unused;
            copy_file(src_dir, dst_dir, "cross_toolchain.txt", out unused);
        }

        string? err2;
        if (!copy_file(Environment.get_current_dir(),
                        Path.build_filename(prefix, "share", "ewpi"), "common.sh", out err2))
            warning("could not stage common.sh: %s", err2);
    }

    // Sets each package's downloaded/extracted/installed flags from the
    // marker files in its destination directory.
    void refresh_status() {
        foreach (unowned string name in all_names) {
            var pkg = packages[name];
            string dir = Path.build_filename(package_dir_dst, name);
            pkg.downloaded = has_mark(dir, "downloaded");
            pkg.extracted = has_mark(dir, "extracted");
            pkg.installed = has_mark(dir, "installed");
        }
    }

    int count_not_installed() {
        int n = 0;
        foreach (unowned string name in order)
            if (!packages[name].installed) n++;
        return n;
    }

    // -------------------------------------------------------------
    // Dependency resolution
    // -------------------------------------------------------------

    // Post-order DFS: every dependency is placed before the package that
    // needs it, and each package appears in `order` exactly once. Exits
    // the program with a clear message on an unknown dependency or a
    // dependency cycle, rather than silently dropping packages or (for a
    // cycle) recursing forever.
    void resolve_tree(string root) {
        var visited = new HashTable<string, bool>(str_hash, str_equal);
        var on_stack = new HashTable<string, bool>(str_hash, str_equal);
        var path = new Array<string>();
        var result = new Array<string>();
        resolve_tree_visit(root, visited, on_stack, path, result);
        order = {};
        for (int i = 0; i < result.length; i++)
            order += result.index(i);
    }

    void resolve_tree_visit(string name, HashTable<string, bool> visited,
                             HashTable<string, bool> on_stack, Array<string> path,
                             Array<string> result) {
        var pkg = packages[name];
        if (pkg == null) {
            stdout.printf("Unknown dependency '%s' (via %s), exiting...\n",
                          name, path_to_string(path));
            Process.exit(1);
        }

        if (on_stack.contains(name)) {
            path.append_val(name);
            stdout.printf("Dependency cycle detected: %s, exiting...\n", path_to_string(path));
            Process.exit(1);
        }
        if (visited.contains(name))
            return;

        on_stack[name] = true;
        path.append_val(name);

        foreach (unowned string dep in pkg.deps)
            resolve_tree_visit(dep, visited, on_stack, path, result);

        path.remove_index(path.length - 1);
        on_stack.remove(name);

        visited[name] = true;
        result.append_val(name);
    }

    static string path_to_string(Array<string> path) {
        var sb = new StringBuilder();
        for (int i = 0; i < path.length; i++) {
            if (i > 0) sb.append(" -> ");
            sb.append(path.index(i));
        }
        return sb.str;
    }

    void print_pending() {
        stdout.printf("\nPackages (%d)\n", count_not_installed());
        stdout.flush();
        foreach (unowned string name in order) {
            var pkg = packages[name];
            if (!pkg.installed)
                stdout.printf("  %s-%s\n", pkg.name, pkg.version);
        }
        stdout.printf("\n");
        stdout.flush();
    }

    // -------------------------------------------------------------
    // Progress display
    // -------------------------------------------------------------

    void compute_name_field_width() {
        name_field_width = 0;
        foreach (unowned string name in order) {
            var pkg = packages[name];
            int w = pkg.name.length + 1 + pkg.version.length;
            if (w > name_field_width) name_field_width = w;
        }
    }

    // A single "(i/count) name-version [#####     ]  NN%" progress line,
    // redrawn in place with '\r'. Pass name == null to draw the final,
    // label-less 100% line.
    void show_progress(int i, int count, string? name, string? version) {
        const int BAR_WIDTH = 40;
        var line = new StringBuilder();

        line.append_printf("(%2d/%d) ", i + 1, count);

        if (name != null) {
            string label = "%s-%s".printf(name, version);
            line.append(label);
            for (int j = label.length; j < name_field_width; j++)
                line.append_c(' ');
        } else {
            for (int j = 0; j < name_field_width; j++)
                line.append_c(' ');
        }

        int filled = (BAR_WIDTH * (i + 1)) / count;
        int percent = (100 * (i + 1)) / count;

        line.append(" [");
        for (int j = 0; j < BAR_WIDTH; j++)
            line.append_c(j < filled ? '#' : ' ');
        line.append_printf("] %3d%%", percent);

        stdout.printf("\r%s", line.str);
        stdout.flush();
    }

    // -------------------------------------------------------------
    // Download / extract / install / clean / strip / nsis
    // -------------------------------------------------------------

    void download_all() {
        int pending = 0;
        foreach (unowned string name in order) {
            var pkg = packages[name];
            string dst = Path.build_filename(package_dir_dst, name);
            if (has_mark(dst, "downloaded"))
                pkg.downloaded = true;
            else
                pending++;
        }
        if (pending == 0) return;

        stdout.printf(":: Download sources...\n");
        stdout.flush();

        foreach (unowned string name in order) {
            var pkg = packages[name];
            if (pkg.downloaded) continue;

            string dst = Path.build_filename(package_dir_dst, name);
            string err = "";
            bool ok;
            if (pkg.is_git) {
                try {
                    Ggit.init();

                    // Strip ".git" from the URL basename (pkg.tarname) to match git's default folder naming
                    string repo_name = pkg.tarname.has_suffix(".git")
                        ? pkg.tarname.substring(0, pkg.tarname.length - 4)
                        : pkg.tarname;

                    var location = File.new_for_path(Path.build_filename(dst, repo_name));
                    var clone_opts = new Ggit.CloneOptions();

                    Ggit.Repository.clone(pkg.url, location, clone_opts);
                    ok = true;
                } catch (Error e) {
                    err = e.message;
                    ok = false;
                }
            } else {
                try {
                    var message = new Soup.Message("GET", pkg.url);
                    var session = new Soup.Session();
                    session.set("ssl-strict", false);
                    var input_stream = session.send(message, null);

                    var file = File.new_for_path(Path.build_filename(dst, pkg.tarname));
                    var output_stream = file.replace(null, false, FileCreateFlags.NONE, null);

                    // Splice handles the buffered read/write loop automatically
                    output_stream.splice(input_stream,
                        OutputStreamSpliceFlags.CLOSE_SOURCE | OutputStreamSpliceFlags.CLOSE_TARGET,
                        null);
                    ok = true;
                } catch (Error e) {
                    err = e.message;
                    ok = false;
                }
            }
            if (!ok) {
                stdout.printf("error while downloading package %s: %s\n", pkg.name, err);
                Process.exit(1);
            }

            mark(dst, "downloaded");
            if (pkg.is_git)
                mark(dst, "extracted"); // a git clone is already "extracted"
        }
    }

    void extract_all(bool verbose) {
        int pending = 0;
        foreach (unowned string name in order) {
            var pkg = packages[name];
            string dst = Path.build_filename(package_dir_dst, name);
            if (has_mark(dst, "extracted"))
                pkg.extracted = true;
            else
                pending++;
        }
        if (pending == 0) return;

        stdout.printf(":: Extraction of sources...\n");
        stdout.flush();

        int c = 0;
        foreach (unowned string name in order) {
            var pkg = packages[name];
            if (pkg.extracted) continue;

            int dot = pkg.tarname.last_index_of_char('.');
            string ext = (dot >= 0) ? pkg.tarname.substring(dot + 1) : "";

            string[] tar_argv = { "tar" };
            string flags = verbose ? "xv" : "x";
            if (ext == "gz" || ext == "tgz")
                flags += "z";
            else if (ext == "bz2")
                flags += "j";
            else
                flags += "Jh";
            flags += "f";
            tar_argv += flags;
            tar_argv += pkg.tarname;

            show_progress(c, pending, pkg.name, pkg.version);

            string dst = Path.build_filename(package_dir_dst, name);
            string? err;
            if (!spawn_visible(tar_argv, out err, dst)) {
                stdout.printf(" Can not extract %s: %s\n", pkg.tarname, err);
                Process.exit(1);
            }
            mark(dst, "extracted");
            c++;
        }

        show_progress(pending - 1, pending, null, null);
        stdout.printf("\n");
    }

    void install_all(string prefix, string host, string arch, string jobopt,
                      bool verbose, string winver) {
        int pending = count_not_installed();
        if (pending == 0) return;

        stdout.printf("\n:: Installation of packages...\n");
        stdout.flush();

        int c = 0;
        foreach (unowned string name in order) {
            var pkg = packages[name];
            if (pkg.installed) continue;

            show_progress(c, pending, pkg.name, pkg.version);

            string dst = Path.build_filename(package_dir_dst, name);
            string[] argv = {
                "sh", "./install.sh", arch, pkg.tarname, prefix, host,
                (jobopt == "") ? "no" : jobopt, verbose ? "yes" : "no", winver
            };
            string? err;
            if (!spawn_visible(argv, out err, dst)) {
                stdout.printf(" Can not install %s: %s\n", pkg.name, err);
                Process.exit(1);
            }
            mark(dst, "installed");
            c++;
        }

        show_progress(pending - 1, pending, null, null);
        stdout.printf("\n");
    }

    static void remove_recursive(string path) {
        if (path_is_dir(path)) {
            Dir dir;
            try {
                dir = Dir.open(path);
            } catch (Error e) {
                return;
            }
            string? entry;
            while ((entry = dir.read_name()) != null)
                remove_recursive(Path.build_filename(path, entry));
            DirUtils.remove(path);
        } else {
            FileUtils.unlink(path);
        }
    }

    void clean_all() {
        stdout.printf("\n:: Cleaning...\n");
        stdout.flush();

        int count = order.length;
        for (int i = 0; i < count; i++) {
            var pkg = packages[order[i]];
            show_progress(i, count, pkg.name, pkg.version);

            string dst = Path.build_filename(package_dir_dst, pkg.name);
            Dir dir;
            try {
                dir = Dir.open(dst);
                string? entry;
                while ((entry = dir.read_name()) != null) {
                    string sub = Path.build_filename(dst, entry);
                    if (path_is_dir(sub))
                        remove_recursive(sub);
                }
            } catch (Error e) {
                // nothing to clean
            }
            FileUtils.unlink(Path.build_filename(dst, pkg.tarname));
        }

        show_progress(count - 1, count, null, null);
        stdout.printf("\n");
    }

    static void strip_dir(string path, string strip_tool) {
        Dir dir;
        try {
            dir = Dir.open(path);
        } catch (Error e) {
            return;
        }

        string? entry;
        while ((entry = dir.read_name()) != null) {
            string full = Path.build_filename(path, entry);
            if (path_is_dir(full)) {
                strip_dir(full, strip_tool);
            } else if (full.has_suffix(".dll")) {
                stdout.printf("  %s\n", full);
                stdout.flush();
                string? err;
                if (!spawn_visible({ strip_tool, full }, out err))
                    stdout.printf("can not strip '%s': %s\n", full, err);
            }
        }
    }

    void strip_all(string prefix, string host) {
        stdout.printf("\n:: Stripping DLL...\n");
        stdout.flush();

        string strip_tool = "%s-strip".printf(host);
        strip_dir(Path.build_filename(prefix, "bin"), strip_tool);
        strip_dir(Path.build_filename(prefix, "lib"), strip_tool);

        stdout.printf("\n");
    }

    void build_nsis_installer(string prefix, string host, string winver, bool efl) {
        strip_all(prefix, host);

        string arch = (host == "i686-w64-mingw32") ? "i686" : "x86_64";
        string arch_suffix = (host == "i686-w64-mingw32") ? "32" : "64";
        string script = efl ? "./efl_nsis.sh" : "./ewpi_nsis.sh";

        stdout.printf("\n:: Create %s NSIS installer...\n", efl ? "EFL" : "EWPI");
        stdout.flush();

        string[] argv = {
            "sh", script, prefix, "%d.%d".printf(VMAJ, VMIN), arch, arch_suffix, winver
        };
        string? err;
        if (!spawn_visible(argv, out err))
            stdout.printf(" Can not create NSIS installer: %s\n", err);

        stdout.printf("\n");
    }

    // -------------------------------------------------------------
    // Orchestration
    // -------------------------------------------------------------

    public int run(string prefix, string host, string arch, string jobopt,
                    string winver, bool strip, bool nsis, bool verbose,
                    bool efl, bool cleaning) {
        stdout.printf(":: Configuration...\n");
        stdout.printf("  prefix:    %s\n", prefix);
        stdout.printf("  host:      %s\n", host);
        stdout.printf("  arch:      %s\n", arch);
        stdout.printf("  strip:     %s\n", strip ? "yes" : "no");
        stdout.printf("  installer: %s\n", nsis ? "yes" : "no");
        stdout.printf("  verbose:   %s\n", verbose ? "yes" : "no");
        stdout.printf("  efl:       %s\n", efl ? "yes" : "no");
        stdout.printf("  jobs:      %s\n", jobopt);
        stdout.printf("\n");
        stdout.flush();

        stdout.printf(":: Checking requirements...\n");
        if (!check_requirements(host)) {
            stdout.printf("one of the requirements is not found, exiting...\n");
            return 1;
        }

        stdout.printf(":: Prepare directories in %s...\n", prefix);
        set_package_dirs(prefix);

        if (!load_packages())
            return 1;
        sync_dst_dirs(prefix);

        stdout.printf(":: Check which package is not installed...\n");
        stdout.flush();
        refresh_status();

        stdout.printf(":: Build the dependency tree...\n");
        stdout.flush();
        resolve_tree("efl");
        if (!efl && order.length > 0 && order[order.length - 1] == "efl")
            order = order[0:order.length - 1];

        print_pending();
        download_all();
        compute_name_field_width();
        extract_all(verbose);
        install_all(prefix, host, arch, jobopt, verbose, winver);

        if (strip && !nsis)
            strip_all(prefix, host);
        if (nsis)
            build_nsis_installer(prefix, host, winver, efl);
        if (cleaning)
            clean_all();

        return 0;
    }
}

// ---------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------

// GLib.OptionContext gives us "--opt=value" and "--opt value" for free,
// automatic type coercion, and a proper error (not a silent "unknown
// option -> print_usage") for things like "--jobz=4" or a stray "-x". We
// still hand-validate the two-choice options (--host, --winver), since
// OptionContext has no notion of an enum argument.
int main(string[] args) {
    string? prefix = null;
    string host = "x86_64-w64-mingw32";
    string? arch = null;
    string jobopt = "";
    string winver = "win10";
    bool strip = false;
    bool nsis = false;
    bool verbose = false;
    bool efl = false;
    bool cleaning = false;
    bool show_help = false;
    bool show_version = false;

    OptionEntry[] entries = {
        { "help", 0, 0, OptionArg.NONE, ref show_help,
          "show this help message and exit", null },
        { "version", 0, 0, OptionArg.NONE, ref show_version,
          "show the Ewpi version and exit", null },
        { "prefix", 0, 0, OptionArg.FILENAME, ref prefix,
          "install in DIR (must be an absolute path) [default=$HOME/ewpi_$arch]", "DIR" },
        { "host", 0, 0, OptionArg.STRING, ref host,
          "host triplet, either i686-w64-mingw32 or x86_64-w64-mingw32 [default=x86_64-w64-mingw32]",
          "VAL" },
        { "arch", 0, 0, OptionArg.STRING, ref arch,
          "value passed to -march and -mtune gcc options [default=i686|x86-64]", "VAL" },
        { "winver", 0, 0, OptionArg.STRING, ref winver,
          "requested Windows version, win7 or win10 [default=win10]", "VAL" },
        { "verbose", 0, 0, OptionArg.NONE, ref verbose, "verbose mode", null },
        { "strip", 0, 0, OptionArg.NONE, ref strip, "strip DLL", null },
        { "nsis", 0, 0, OptionArg.NONE, ref nsis, "strip DLL and create the NSIS installer", null },
        { "efl", 0, 0, OptionArg.NONE, ref efl, "install the EFL", null },
        { "jobs", 0, 0, OptionArg.STRING, ref jobopt,
          "maximum number of used jobs [default=maximum]", "VAL" },
        { "clean", 0, 0, OptionArg.NONE, ref cleaning,
          "remove the archives and the created directories (not removed by default)", null },
        { null }
    };

    var context = new OptionContext("- compile and install the EFL dependencies");
    context.set_help_enabled(false); // we print our own --help, matching the original wording
    context.set_summary(
        "Examples:\n" +
        "  ./ewpi --prefix=/opt/ewpi_32 --host=i686-w64-mingw32\n" +
        "  ./ewpi --host=x86_64-w64-mingw32 --efl --jobs=4");
    context.add_main_entries(entries, null);

    try {
        context.parse(ref args);
    } catch (OptionError e) {
        stdout.printf("%s\n\n", e.message);
        stdout.printf("%s", context.get_help(true, null));
        return 1;
    }

    if (show_help) {
        stdout.printf("%s", context.get_help(true, null));
        return 0;
    }
    if (show_version) {
        stdout.printf("Ewpi version %d.%d\n", Ewpi.VMAJ, Ewpi.VMIN);
        return 0;
    }

    // Anything left in `args` past argv[0] is a positional argument this
    // program doesn't take.
    if (args.length > 1) {
        stdout.printf("unexpected argument '%s'\n\n", args[1]);
        stdout.printf("%s", context.get_help(true, null));
        return 1;
    }

    if (host != "i686-w64-mingw32" && host != "x86_64-w64-mingw32") {
        stdout.printf("--host must be i686-w64-mingw32 or x86_64-w64-mingw32\n\n");
        stdout.printf("%s", context.get_help(true, null));
        return 1;
    }
    if (winver != "win7" && winver != "win10") {
        stdout.printf("--winver must be win7 or win10\n\n");
        stdout.printf("%s", context.get_help(true, null));
        return 1;
    }

    bool is_32 = (host == "i686-w64-mingw32");

    if (prefix == null)
        prefix = Path.build_filename(
            Environment.get_home_dir(), is_32 ? "ewpi_32" : "ewpi_64");
    if (arch == null)
        arch = is_32 ? "i686" : "x86-64";

    // Normalize to forward slashes and drop a trailing slash.
    prefix = prefix.replace("\\", "/");
    if (prefix.has_suffix("/"))
        prefix = prefix.substring(0, prefix.length - 1);

    if (!Path.is_absolute(prefix)) {
        stdout.printf("prefix must be an absolute path, exiting...\n");
        return 1;
    }

    var ewpi = new Ewpi();
    return ewpi.run(prefix, host, arch, jobopt, winver, strip, nsis, verbose, efl, cleaning);
}
