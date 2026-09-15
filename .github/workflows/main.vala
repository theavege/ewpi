/*
 * main.vala — build a Windows NSIS installer for EFL (or any other MSYS2
 * mingw-w64 package) by vendoring MSYS2's own prebuilt binaries, entirely
 * from Linux.
 *
 * This is a deliberately separate program from ewpi.vala, not a mode
 * bolted onto it: ewpi builds EFL and its dependencies FROM SOURCE,
 * resolving a hand-maintained packages/*.ewpi dependency graph. This
 * tool instead resolves the REAL dependency graph out of a live pacman
 * repository database and downloads finished binaries — a different
 * input (a repo name + package name, not a directory of descriptors)
 * and a different algorithm (walk an actual %DEPENDS% graph rather than
 * a hand-written deps: line), even though it reuses the same two
 * backend ideas — libsoup3 for fetching, libarchive for extracting —
 * because those are simply the right tools for both jobs.
 *
 * ALGORITHM
 * ---------
 * A pacman repository database (e.g. mingw64.db) is itself just a
 * gzipped tar archive: one directory per package ("name-version/"),
 * each containing a "desc" file and a "depends" file in a simple
 * "%FIELD%\nvalue\n\n%FIELD%\nvalue\n..." text format. Every file
 * belonging to the same package directory is concatenated before
 * parsing — this is deliberately layout-agnostic (works whether a
 * given repo splits desc/depends into two files or combines them into
 * one) rather than hard-coding an assumption about pacman's on-disk
 * format that could change.
 *
 * From the resulting name -> {version, filename, depends[]} map, the
 * dependency closure is a plain graph reachability walk (BFS): visit
 * a package, queue up every %DEPENDS% entry not already seen, repeat
 * until the queue is empty. This is deliberately NOT a topological
 * sort — an earlier version of this tool used one (the same DFS
 * algorithm as ewpi.vala's own resolve_tree(), which genuinely needs
 * a build order since a from-source build really can't compile a
 * dependent before its dependency exists), and it was the wrong
 * choice here: real MSYS2 packages do contain mutual runtime
 * dependencies (libtiff and libwebp each depend on the other, since
 * each can read/write the other's format) which a topological sort
 * has to reject as a cycle. Vendoring finished binaries has no such
 * ordering constraint — every package is just a .pkg.tar.zst to
 * download and extract into a shared directory, and that works in
 * any order — so a cycle here isn't an error, just a normal shape a
 * real dependency graph can have. A visited set is all that's needed
 * to guarantee termination and dedupe a diamond dependency.
 *
 * Once resolved, each package's .pkg.tar.zst is downloaded and
 * extracted into one shared staging directory (their internal
 * "mingw64/..." payload paths naturally merge across packages, the
 * same way pacman's own file layout works), and a minimal NSIS script
 * is generated and run against that staging directory to produce the
 * final installer .exe.
 *
 * Build:
 *   valac --pkg glib-2.0 --pkg gio-2.0 --pkg posix \
 *         --pkg libsoup-3.0 --pkg libarchive \
 *         -o main main.vala
 *
 * Usage:
 *   ./main <repo> <package> <staging-dir> [installer-name]
 *
 * Example:
 *   ./main mingw64 mingw-w64-x86_64-efl ./efl-staging efl-installer
 *
 * <repo> is one of mingw64/ucrt64/clang64 (whichever environment you
 * want; don't mix packages resolved from different repos — they are
 * not ABI-compatible with each other). <package> must already carry
 * the matching prefix (mingw-w64-x86_64-, mingw-w64-ucrt-x86_64-, or
 * mingw-w64-clang-x86_64-) since that's its real name in the database.
 */

using GLib;
using Soup;
using Archive;

// One package's worth of information out of the repo database.
public class DbEntry : Object {
    public string name;
    public string version;
    public string filename;
    public string[] depends = {};
}

public class Vendor : Object {
    string mirror_base;
    string repo;
    internal HashTable<string, DbEntry> db = new HashTable<string, DbEntry>(str_hash, str_equal);

    public Vendor(string repo) {
        this.repo = repo;
        this.mirror_base = "https://mirror.msys2.org/mingw/%s".printf(repo);
    }

    // -----------------------------------------------------------
    // Fetching (libsoup3) — same shape as ewpi.vala's own
    // download_file()/fetch_text(): a generous per-socket timeout
    // (this is a batch tool, not an interactive one) and a small
    // retry loop for the transient failures a real mirror network
    // can hand back (a 502 from a flaky edge node, a timeout on a
    // slow link) — see ewpi.vala's own history in this project for
    // why that retry loop earns its keep.
    // -----------------------------------------------------------

    const uint SOCKET_TIMEOUT_SECONDS = 300;
    const int MAX_ATTEMPTS = 5;
    const uint MAX_RETRY_DELAY_SECONDS = 16;

    enum FetchOutcome { OK, RETRY, FATAL }

    FetchOutcome fetch_once(string url, out InputStream? stream, out string? error_out) {
        stream = null;
        error_out = null;

        var session = new Soup.Session();
        session.timeout = SOCKET_TIMEOUT_SECONDS;
        var msg = new Soup.Message("GET", url);
        if (msg == null) {
            error_out = "invalid URL '%s'".printf(url);
            return FetchOutcome.FATAL;
        }

        try {
            stream = session.send(msg);
        } catch (Error e) {
            error_out = e.message;
            return FetchOutcome.RETRY;
        }

        uint status = msg.get_status();
        if (status >= 300) {
            error_out = "HTTP %u %s".printf(status, msg.get_reason_phrase());
            bool transient = status == 429 || status == 408 || (status >= 500 && status <= 504);
            return transient ? FetchOutcome.RETRY : FetchOutcome.FATAL;
        }

        return FetchOutcome.OK;
    }

    // Downloads `url` straight to `dest_path` with the retry policy above.
    bool download_to_file(string url, string dest_path, out string? error_out) {
        error_out = null;
        for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
            InputStream? in_stream;
            string? attempt_err;
            var outcome = fetch_once(url, out in_stream, out attempt_err);

            if (outcome == FetchOutcome.OK) {
                try {
                    var out_file = File.new_for_path(dest_path);
                    var out_stream = out_file.replace(null, false, FileCreateFlags.REPLACE_DESTINATION);
                    out_stream.splice(in_stream,
                        OutputStreamSpliceFlags.CLOSE_SOURCE | OutputStreamSpliceFlags.CLOSE_TARGET);
                    return true;
                } catch (Error e) {
                    attempt_err = e.message;
                    outcome = FetchOutcome.RETRY;
                }
            }

            error_out = attempt_err;
            if (outcome == FetchOutcome.FATAL || attempt == MAX_ATTEMPTS)
                return false;

            uint delay = uint.min(2u << (attempt - 1), MAX_RETRY_DELAY_SECONDS);
            stdout.printf("  %s — retrying in %us (attempt %d/%d)...\n",
                          attempt_err, delay, attempt + 1, MAX_ATTEMPTS);
            stdout.flush();
            Thread<void*>.usleep(delay * 1000000);
        }
        return false;
    }

    // -----------------------------------------------------------
    // Extraction (libarchive) — identical approach to ewpi.vala's
    // extract_archive(): support_filter_all()/support_format_all()
    // auto-detect gzip/zstd/... and the container format, so this
    // one routine handles both the gzipped repo database and the
    // zstd-compressed .pkg.tar.zst package archives with no branching
    // on file extension.
    // -----------------------------------------------------------

    bool extract_archive(string src_path, string dest_dir, out string? error_out) {
        error_out = null;

        var reader = new Archive.Read();
        reader.support_filter_all();
        reader.support_format_all();

        var writer = new Archive.WriteDisk();
        writer.set_options(Archive.ExtractFlags.TIME | Archive.ExtractFlags.PERM |
                            Archive.ExtractFlags.ACL | Archive.ExtractFlags.FFLAGS);
        writer.set_standard_lookup();

        if (reader.open_filename(src_path, 10240) != Archive.Result.OK) {
            error_out = reader.error_string();
            return false;
        }

        while (true) {
            unowned Archive.Entry entry;
            var r = reader.next_header(out entry);
            if (r == Archive.Result.EOF)
                break;
            if (r != Archive.Result.OK && r != Archive.Result.WARN) {
                error_out = reader.error_string();
                reader.close();
                writer.close();
                return false;
            }

            entry.set_pathname(Path.build_filename(dest_dir, entry.pathname()));

            // A hard-link entry's own pathname is where the link
            // itself goes — already rewritten above — but its
            // *target* (the file it should point at) is a separate
            // field, and libarchive resolves it exactly as written in
            // the archive: relative to the current working directory,
            // not to dest_dir. Left alone, every hard link in an
            // archive that has any (glibc/Tcl/etc. man pages are full
            // of them — many function names are literally the same
            // file) fails with "Hard-link target ... does not exist"
            // and is silently skipped rather than actually created.
            unowned string? hardlink_target = entry.hardlink();
            if (hardlink_target != null && hardlink_target != "")
                entry.set_hardlink(Path.build_filename(dest_dir, hardlink_target));

            r = writer.write_header(entry);
            if (r != Archive.Result.OK && r != Archive.Result.WARN) {
                warning("%s: %s", src_path, writer.error_string());
                continue;
            }

            unowned uint8[] buf;
            Archive.int64_t offset;
            while (true) {
                r = reader.read_data_block(out buf, out offset);
                if (r == Archive.Result.EOF)
                    break;
                if (r != Archive.Result.OK) {
                    error_out = reader.error_string();
                    reader.close();
                    writer.close();
                    return false;
                }
                writer.write_data_block(buf, offset);
            }
            writer.finish_entry();
        }

        reader.close();
        writer.close();
        return true;
    }

    // Reads every regular file's content out of an archive without
    // extracting to disk — used for the repo database, which we only
    // need to parse, not keep around as files.
    bool read_all_entries(string src_path, out HashTable<string, string>? files_out,
                           out string? error_out) {
        files_out = null;
        error_out = null;

        var reader = new Archive.Read();
        reader.support_filter_all();
        reader.support_format_all();

        if (reader.open_filename(src_path, 10240) != Archive.Result.OK) {
            error_out = reader.error_string();
            return false;
        }

        var files = new HashTable<string, string>(str_hash, str_equal);

        while (true) {
            unowned Archive.Entry entry;
            var r = reader.next_header(out entry);
            if (r == Archive.Result.EOF)
                break;
            if (r != Archive.Result.OK && r != Archive.Result.WARN) {
                error_out = reader.error_string();
                reader.close();
                return false;
            }

            var content = new StringBuilder();
            unowned uint8[] buf;
            Archive.int64_t offset;
            while (true) {
                r = reader.read_data_block(out buf, out offset);
                if (r == Archive.Result.EOF)
                    break;
                if (r != Archive.Result.OK) {
                    error_out = reader.error_string();
                    reader.close();
                    return false;
                }
                content.append_len((string) buf, buf.length);
            }

            files[entry.pathname()] = content.str;
        }

        reader.close();
        files_out = files;
        return true;
    }

    // -----------------------------------------------------------
    // Repo database parsing
    // -----------------------------------------------------------

    // Vala doesn't allow string[] as a generic type argument directly,
    // hence this one-field wrapper around the values a field can hold.
    public class FieldValues : Object {
        public string[] values;
    }

    // Splits "%FIELD%\nvalue1\nvalue2\n\n%FIELD2%\n...\n\n" text into
    // field -> [values]. This is the real pacman desc/depends field
    // format, unchanged since it's a plain, stable text format that
    // predates any of the packages it's describing.
    internal HashTable<string, FieldValues> parse_fields(string text) {
        var fields = new HashTable<string, FieldValues>(str_hash, str_equal);
        foreach (unowned string block in text.split("\n\n")) {
            string[] lines = block.strip().split("\n");
            if (lines.length < 1 || !lines[0].has_prefix("%") || !lines[0].has_suffix("%"))
                continue;
            string field = lines[0].substring(1, lines[0].length - 2);
            var fv = new FieldValues();
            fv.values = lines[1:lines.length];
            fields[field] = fv;
        }
        return fields;
    }

    // Strips a version constraint off a dependency string —
    // "freetype>=2.9" and "freetype=2.9-1" both resolve to "freetype".
    // Real package names in this repo's namespace never contain these
    // operator characters, so the first occurrence is always the cut
    // point.
    internal string dep_name(string raw) {
        var re = /[<>=]/;
        int idx = -1;
        MatchInfo mi;
        if (re.match(raw, 0, out mi)) {
            int start;
            mi.fetch_pos(0, out start, null);
            idx = start;
        }
        return idx >= 0 ? raw.substring(0, idx) : raw;
    }

    // Downloads and parses this.repo's sync database into `db`. Every
    // file under a given "name-version/" top-level directory in the
    // archive is concatenated before field-parsing, so this doesn't
    // care whether desc/depends are one file or two.
    bool load_database(out string? error_out) {
        error_out = null;

        string db_url = "%s/%s.db".printf(mirror_base, repo);
        string db_path = "/tmp/%s.db.tmp".printf(repo);

        stdout.printf(":: Fetching repo database %s...\n", db_url);
        stdout.flush();
        if (!download_to_file(db_url, db_path, out error_out))
            return false;

        HashTable<string, string>? files;
        if (!read_all_entries(db_path, out files, out error_out))
            return false;
        FileUtils.unlink(db_path);

        // Group every file's content by its top-level "name-version/"
        // directory component.
        var grouped = new HashTable<string, StringBuilder>(str_hash, str_equal);
        var iter = HashTableIter<string, string>(files);
        unowned string path;
        unowned string content;
        while (iter.next(out path, out content)) {
            int slash = path.index_of_char('/');
            string group = slash >= 0 ? path.substring(0, slash) : path;
            if (!grouped.contains(group))
                grouped[group] = new StringBuilder();
            grouped[group].append(content);
            grouped[group].append("\n\n"); // guarantee a field separator between files
        }

        var group_iter = HashTableIter<string, StringBuilder>(grouped);
        unowned string group_name;
        unowned StringBuilder group_text;
        while (group_iter.next(out group_name, out group_text)) {
            var fields = parse_fields(group_text.str);

            FieldValues? name_field = fields["NAME"];
            FieldValues? filename_field = fields["FILENAME"];
            FieldValues? version_field = fields["VERSION"];
            if (name_field == null || name_field.values.length == 0 ||
                filename_field == null || filename_field.values.length == 0)
                continue; // not a package directory we can use

            var entry = new DbEntry();
            entry.name = name_field.values[0];
            entry.filename = filename_field.values[0];
            entry.version = (version_field != null && version_field.values.length > 0)
                ? version_field.values[0] : "";

            FieldValues? depends_field = fields["DEPENDS"];
            if (depends_field != null) {
                string[] deps = {};
                foreach (unowned string raw in depends_field.values)
                    deps += dep_name(raw);
                entry.depends = deps;
            }

            db[entry.name] = entry;
        }

        stdout.printf("   %u packages in %s\n", db.size(), repo);
        return true;
    }

    // -----------------------------------------------------------
    // Dependency resolution — plain graph reachability (BFS): visit a
    // package, queue every %DEPENDS% entry not already seen, repeat.
    // No per-path cycle tracking, because a cycle isn't an error for
    // this job — see the file-level comment for why that's a
    // deliberate change from a topological sort, not an oversight.
    // -----------------------------------------------------------

    internal string[] resolve_closure(string root) {
        var visited = new HashTable<string, bool>(str_hash, str_equal);
        var order = new Array<string>();
        var queue = new GLib.Queue<string>();

        queue.push_tail(root);
        visited[root] = true;

        while (!queue.is_empty()) {
            string name = queue.pop_head();
            var entry = db[name];
            if (entry == null) {
                // Not every %DEPENDS% entry names a literal package —
                // pacman also resolves against %PROVIDES% (virtual
                // packages), which this tool doesn't parse. Skip rather
                // than fail outright, since most of these are optional
                // runtime bits (a specific codec backend, etc.), not
                // the package itself — but see this tool's notes on
                // 'cc-libs' specifically: that one is NOT safe to just
                // shrug off, since it's the compiler runtime DLLs.
                stdout.printf("   note: '%s' not found in %s (virtual/provides package? skipping)\n",
                              name, repo);
                continue;
            }

            order.append_val(name);

            foreach (unowned string dep in entry.depends) {
                if (!visited.contains(dep)) {
                    visited[dep] = true;
                    queue.push_tail(dep);
                }
            }
        }

        string[] result = {};
        for (int i = 0; i < order.length; i++)
            result += order.index(i);
        return result;
    }

    // -----------------------------------------------------------
    // Vendoring: download + extract every resolved package into one
    // shared staging directory.
    // -----------------------------------------------------------

    // Deliberately small: this is a shared, community-run mirror
    // network, not a dedicated CDN. Enough concurrency to actually hide
    // per-request latency (including the retry backoff in
    // download_to_file()) behind other requests in flight, not so much
    // that we're hammering it — pacman's own default ParallelDownloads
    // setting is in this same small-single-digit range for the same
    // reason.
    const int MAX_PARALLEL_DOWNLOADS = 6;

    Mutex print_mutex = Mutex();

    // One download's worth of work handed to a thread-pool worker; the
    // result fields are only written by the one worker that owns this
    // job and only read by the main thread after every job has
    // finished (ThreadPool.free with wait=true blocks until then), so
    // no locking is needed on ok/error themselves.
    class DownloadJob : Object {
        public string name;
        public string url;
        public string archive_path;
        public bool ok = false;
        public string? error;
    }

    public bool vendor(string root_package, string staging_dir, out string[]? order_out) {
        order_out = null;

        string? err;
        if (!load_database(out err)) {
            stdout.printf("could not load %s's repo database: %s\n", repo, err);
            return false;
        }

        if (db[root_package] == null) {
            stdout.printf("'%s' is not in the %s repository " +
                          "(check the exact name at https://packages.msys2.org)\n",
                          root_package, repo);
            return false;
        }

        string[] order = resolve_closure(root_package);

        stdout.printf("\n:: Resolved %d packages:\n", order.length);
        foreach (unowned string name in order)
            stdout.printf("  %s (%s)\n", name, db[name].version);
        stdout.printf("\n");

        DirUtils.create_with_parents(staging_dir, 0755);
        string cache_dir = Path.build_filename(staging_dir, ".pkg-cache");
        DirUtils.create_with_parents(cache_dir, 0755);

        // --- Phase 1: download every not-already-cached package, in
        // parallel. Anything already sitting in .pkg-cache from a
        // previous run is skipped here in the main thread before any
        // work is dispatched, same as the old sequential version. ---

        var jobs = new DownloadJob[0];
        foreach (unowned string name in order) {
            var entry = db[name];
            string archive_path = Path.build_filename(cache_dir, entry.filename);
            if (FileUtils.test(archive_path, FileTest.IS_REGULAR))
                continue;

            var job = new DownloadJob();
            job.name = name;
            job.url = "%s/%s".printf(mirror_base, entry.filename);
            job.archive_path = archive_path;
            jobs += job;
        }

        if (jobs.length > 0) {
            int workers = int.min(MAX_PARALLEL_DOWNLOADS, jobs.length);
            stdout.printf(":: Downloading %d package(s) using %d parallel worker(s)...\n",
                          jobs.length, workers);
            stdout.flush();

            try {
                ThreadPoolFunc<DownloadJob> worker = (job) => {
                    string? derr;
                    bool ok = download_to_file(job.url, job.archive_path, out derr);
                    job.ok = ok;
                    job.error = derr;

                    print_mutex.lock();
                    if (ok)
                        stdout.printf("  done: %s\n", job.name);
                    else
                        stdout.printf("  FAILED: %s — %s\n", job.name, derr);
                    stdout.flush();
                    print_mutex.unlock();
                };
                var pool = new ThreadPool<DownloadJob>.with_owned_data(worker, workers, false);

                foreach (var job in jobs)
                    pool.add(job);

                // Blocks until every queued job has actually run.
                ThreadPool<DownloadJob>.free((owned) pool, false, true);
            } catch (ThreadError e) {
                stdout.printf("could not start download thread pool: %s\n", e.message);
                return false;
            }

            string[] failed = {};
            foreach (var job in jobs)
                if (!job.ok)
                    failed += "%s: %s".printf(job.name, job.error);

            if (failed.length > 0) {
                stdout.printf("\n%d of %d download(s) failed:\n", failed.length, jobs.length);
                foreach (unowned string f in failed)
                    stdout.printf("  %s\n", f);
                return false;
            }

            stdout.printf("\n");
        }

        // --- Phase 2: extraction, sequential. This is disk/CPU-bound
        // rather than network-bound, so it doesn't get the same payoff
        // from parallelizing that the downloads do, and every package
        // extracts into the same shared staging_dir — keeping this
        // single-threaded avoids having to reason about concurrent
        // directory creation for no real speed benefit. ---

        int i = 0;
        foreach (unowned string name in order) {
            i++;
            var entry = db[name];
            string archive_path = Path.build_filename(cache_dir, entry.filename);

            stdout.printf("(%d/%d) extracting %s\n", i, order.length, entry.filename);
            stdout.flush();

            string? eerr;
            if (!extract_archive(archive_path, staging_dir, out eerr)) {
                stdout.printf("  extraction failed: %s\n", eerr);
                return false;
            }
        }

        order_out = order;
        return true;
    }
}

// -----------------------------------------------------------
// NSIS packaging
// -----------------------------------------------------------

// A minimal, generic "install everything under this directory" NSIS
// script — deliberately simple; if you need per-component sections,
// a Start Menu shortcut, a license page, etc., this is a starting
// point to extend, not a finished installer UI.
string generate_nsi(string staging_dir, string installer_name, string app_name) {
    var sb = new StringBuilder();
    sb.append_printf("OutFile \"%s.exe\"\n", installer_name);
    sb.append_printf("Name \"%s\"\n", app_name);
    sb.append("InstallDir \"$PROGRAMFILES64\\%s\"\n".printf(app_name));
    sb.append("RequestExecutionLevel admin\n\n");
    sb.append("Section \"Install\"\n");
    sb.append("  SetOutPath \"$INSTDIR\"\n");
    // Trailing backslash matters to NSIS's File /r glob semantics.
    sb.append_printf("  File /r \"%s\\*.*\"\n", staging_dir.replace("/", "\\"));
    sb.append("  WriteUninstaller \"$INSTDIR\\uninstall.exe\"\n");
    sb.append("SectionEnd\n\n");
    sb.append("Section \"Uninstall\"\n");
    sb.append("  RMDir /r \"$INSTDIR\"\n");
    sb.append("SectionEnd\n");
    return sb.str;
}

string to_absolute_path(string path) {
    return Path.is_absolute(path) ? path : Path.build_filename(Environment.get_current_dir(), path);
}

bool run_makensis(string nsi_path, out string? error_out) {
    error_out = null;
    try {
        var launcher = new SubprocessLauncher(SubprocessFlags.NONE);
        var proc = launcher.spawnv({ "makensis", nsi_path });
        proc.wait();
        if (!proc.get_successful()) {
            error_out = proc.get_if_signaled()
                ? "killed by signal %d".printf(proc.get_term_sig())
                : "exited with status %d".printf(proc.get_exit_status());
            return false;
        }
        return true;
    } catch (Error e) {
        error_out = e.message;
        return false;
    }
}

int main(string[] args) {
    // True compile-time literals — these are exactly what Vala's const
    // supports. staging_dir's default below can't be one of these: it
    // needs Environment.get_home_dir() at runtime, which const doesn't
    // allow (same restriction as C's const — a literal or a constant
    // expression of literals, not a function call).
    const string DEFAULT_REPO = "ucrt64";
    const string DEFAULT_PACKAGE = "mingw-w64-ucrt-x86_64-efl";
    const string DEFAULT_INSTALLER_NAME = "installer";

    if (args.length > 1 && (args[1] == "--help" || args[1] == "-h")) {
        stdout.printf("Usage: %s [repo] [package] [staging-dir] [installer-name]\n\n", args[0]);
        stdout.printf("  [repo]            mingw64, ucrt64, or clang64 (default: %s)\n", DEFAULT_REPO);
        stdout.printf("  [package]         e.g. mingw-w64-ucrt-x86_64-efl (must match [repo]'s prefix)\n");
        stdout.printf("                      (default: %s)\n", DEFAULT_PACKAGE);
        stdout.printf("  [staging-dir]     where to vendor the extracted packages\n");
        stdout.printf("                      (default: $HOME/efl-staging)\n");
        stdout.printf("  [installer-name]  output .exe base name (default: %s)\n", DEFAULT_INSTALLER_NAME);
        return 0;
    }

    string default_staging_dir = Path.build_filename(Environment.get_home_dir(), "efl-staging");

    string repo = args.length > 1 ? args[1] : DEFAULT_REPO;
    string package = args.length > 2 ? args[2] : DEFAULT_PACKAGE;
    string staging_dir = args.length > 3 ? args[3] : default_staging_dir;
    string installer_name = args.length > 4 ? args[4] : DEFAULT_INSTALLER_NAME;

    if (repo != "mingw64" && repo != "ucrt64" && repo != "clang64") {
        stdout.printf("repo must be one of: mingw64, ucrt64, clang64\n");
        return 1;
    }

    var vendor = new Vendor(repo);
    string[]? order;
    if (!vendor.vendor(package, staging_dir, out order))
        return 1;

    stdout.printf(":: Generating NSIS script...\n");
    string nsi_path = Path.build_filename(Environment.get_current_dir(), installer_name + ".nsi");
    string nsi_content = generate_nsi(to_absolute_path(staging_dir), installer_name, package);
    try {
        FileUtils.set_contents(nsi_path, nsi_content);
    } catch (Error e) {
        stdout.printf("could not write %s: %s\n", nsi_path, e.message);
        return 1;
    }

    stdout.printf(":: Running makensis...\n");
    string? merr;
    if (!run_makensis(nsi_path, out merr)) {
        stdout.printf("makensis failed: %s\n", merr);
        stdout.printf("(the staged files are still in %s if you want to inspect or repackage them by hand)\n",
                      staging_dir);
        return 1;
    }

    stdout.printf(":: Wrote %s.exe\n", installer_name);
    return 0;
}
