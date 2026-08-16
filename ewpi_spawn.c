#include <string.h>

#ifdef _WIN32
# include <windows.h>
# include <stdio.h>
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

int ewpi_spawn(const char *host, const char *prog, const char *option)
{
    char cmd[256];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE pipe_out_read;
    HANDLE pipe_out_write;
    HANDLE pipe_err_read;
    HANDLE pipe_err_write;
    DWORD exit_code;
    BOOL res;

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&pipe_out_read, &pipe_out_write, &sa, 0))
        return 0;

    if (!SetHandleInformation(pipe_out_read, HANDLE_FLAG_INHERIT, 0) )
        goto close_pipe_out;

    if (!CreatePipe(&pipe_err_read, &pipe_err_write, &sa, 0))
        goto close_pipe_out;

    if (!SetHandleInformation(pipe_err_read, HANDLE_FLAG_INHERIT, 0))
        goto close_pipe_err;

    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = pipe_err_write;
    si.hStdOutput = pipe_out_write;

    if (snprintf(cmd, sizeof(cmd), "%s%s%s %s",
                 host ? host : "", host ? "-" : "", prog, option) >= (int)sizeof(cmd))
        goto close_pipe_err;

    if (!CreateProcess(NULL, cmd, NULL, NULL,
                       TRUE, 0UL, NULL, NULL, &si, &pi))
        goto close_pipe_err;

    /* write ends are now owned by the child; the parent's copies must be
     * closed so the pipe can signal EOF, and the (unused) read ends are
     * closed too since nothing ever reads from them. */
    CloseHandle(pipe_err_write);
    CloseHandle(pipe_out_write);
    CloseHandle(pipe_err_read);
    CloseHandle(pipe_out_read);

    WaitForSingleObject(pi.hProcess, INFINITE);

    res = GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!res)
        return 0;

    if (exit_code != 0UL)
        return 0;

    return 1;

  close_pipe_err:
    CloseHandle(pipe_err_read);
    CloseHandle(pipe_err_write);
  close_pipe_out:
    CloseHandle(pipe_out_read);
    CloseHandle(pipe_out_write);

    return 0;
}

#else

int ewpi_spawn(const char *host, const char *prog, const char *option)
{
    pid_t pid;

    pid = fork();
    if (pid < 0)
        return 0;

    if (pid == 0)
    {
        /* child */
        char cmd[256];
        char *args[3];
        int fd;

        fd = open("/dev/null", O_WRONLY);
        if (fd <  0) exit(1);
        if (dup2(fd, 1) < 0) exit(1);
        if (dup2(fd, 2) < 0) exit(1);
        if (close(fd) < 0) exit(1);;

        if (snprintf(cmd, sizeof(cmd), "%s%s%s",
                     host ? host : "", host ? "-" : "", prog) >= (int)sizeof(cmd))
            exit(1);
        args[0] = cmd;
        args[1] = (char *)option;
        args[2] = NULL;
        execvp(args[0], args);
        exit(errno);
    }
    else
    {
        /* parent */
        int status;
        int ret;

        ret = waitpid(pid, &status, 0);
        if (ret != 0)
            return 0;

        return WIFEXITED(status) ? WEXITSTATUS(status) == 0 : 0;
    }
}

#endif
