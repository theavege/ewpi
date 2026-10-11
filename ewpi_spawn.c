#include <string.h>

#ifdef _WIN32
# include <stdio.h>
# include <windows.h>
#else
# include <errno.h>
# include <fcntl.h>
# include <stdio.h>
# include <stdlib.h>
# include <sys/wait.h>
# include <unistd.h>
#endif

#include "ewpi_spawn.h"

#ifdef _WIN32

int ewpi_spawnv(const char *host, const char *prog, const char *arg)
{
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE pipe_out_read = NULL;
    HANDLE pipe_out_write = NULL;
    HANDLE pipe_err_read = NULL;
    HANDLE pipe_err_write = NULL;
    DWORD exit_code = 1;
    BOOL res = FALSE;
    int written;

    if (prog == NULL || *prog == '\0')
        return 0;

    if (arg == NULL)
        arg = "";

    if (host != NULL && *host != '\0')
    {
        written = snprintf(cmd, sizeof(cmd), "%s-%s %s",
                           host, prog, arg);
    }
    else
    {
        written = snprintf(cmd, sizeof(cmd), "%s %s",
                           prog, arg);
    }

    if (written < 0 || (size_t)written >= sizeof(cmd))
        return 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&pipe_out_read, &pipe_out_write, &sa, 0))
        goto cleanup;

    if (!SetHandleInformation(pipe_out_read, HANDLE_FLAG_INHERIT, 0))
        goto cleanup;

    if (!CreatePipe(&pipe_err_read, &pipe_err_write, &sa, 0))
        goto cleanup;

    if (!SetHandleInformation(pipe_err_read, HANDLE_FLAG_INHERIT, 0))
        goto cleanup;

    ZeroMemory(&pi, sizeof(pi));

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = pipe_err_write;
    si.hStdOutput = pipe_out_write;

    if (!CreateProcessA(NULL, cmd, NULL, NULL,
                        TRUE, 0UL, NULL, NULL, &si, &pi))
        goto cleanup;

    CloseHandle(pipe_err_write);
    pipe_err_write = NULL;
    CloseHandle(pipe_out_write);
    pipe_out_write = NULL;

    WaitForSingleObject(pi.hProcess, INFINITE);

    res = GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    pi.hProcess = NULL;

    CloseHandle(pi.hThread);
    pi.hThread = NULL;

    if (!res)
        goto cleanup;

    if (exit_code != 0UL)
        goto cleanup;

    return 1;

cleanup:
    if (pi.hProcess != NULL)
        CloseHandle(pi.hProcess);

    if (pi.hThread != NULL)
        CloseHandle(pi.hThread);

    if (pipe_out_read != NULL)
        CloseHandle(pipe_out_read);

    if (pipe_out_write != NULL)
        CloseHandle(pipe_out_write);

    if (pipe_err_read != NULL)
        CloseHandle(pipe_err_read);

    if (pipe_err_write != NULL)
        CloseHandle(pipe_err_write);

    return 0;
}

int ewpi_spawn(const char *host, const char *prog, const char *option)
{
    return ewpi_spawnv(host, prog, option);
}

#else

int ewpi_spawnv(const char *host, const char *prog, const char *arg)
{
    pid_t pid;

    if (prog == NULL || *prog == '\0')
        return 0;

    pid = fork();
    if (pid < 0)
        return 0;

    if (pid == 0)
    {
        /* child */
        char cmd[256];
        char *args[3];
        int fd;
        int written;

        fd = open("/dev/null", O_WRONLY);
        if (fd <  0) _exit(1);
        if (dup2(fd, 1) < 0) _exit(1);
        if (dup2(fd, 2) < 0) _exit(1);
        if (close(fd) < 0) _exit(1);

        if (host != NULL && *host != '\0')
            written = snprintf(cmd, sizeof(cmd), "%s-%s", host, prog);
        else
            written = snprintf(cmd, sizeof(cmd), "%s", prog);

        if (written < 0 || (size_t)written >= sizeof(cmd))
            _exit(1);

        args[0] = cmd;
        /* NULL or empty arg means: run with no extra argument. */
        args[1] = (arg != NULL && *arg != '\0') ? (char *)arg : NULL;
        args[2] = NULL;
        execvp(args[0], args);
        _exit(errno);
    }
    else
    {
        /* parent */
        int status;
        int ret;

        ret = waitpid(pid, &status, 0);
        if (ret < 0)
            return 0;

        return WIFEXITED(status) ? (WEXITSTATUS(status) == 0) : 0;
    }
}

int ewpi_spawn(const char *host, const char *prog, const char *option)
{
    return ewpi_spawnv(host, prog, option);
}

#endif
