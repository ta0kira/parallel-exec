/* Kevin P. Barry [ta0kira@gmail.com], 24 April 2014 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/wait.h>


#ifndef BUFFER_PAGES
#define BUFFER_PAGES 1
#endif


static void register_handlers(int);


typedef struct
{
	int   socket;
	FILE *file;
} usable_socket;


static int parse_long_value(const char*, int*);
static int process_loop(const char*, int, unsigned int, unsigned int);
static int select_loop(const char*, usable_socket*, unsigned int);

#define USAGE "%s [process count] (buffer size) (command line...)\n"


int main(int argc, char *argv[])
{
	int processes = 0, line_size = 0, I, status;


	//1st arg: number of concurrent processes
	if (argc < 2 || !(parse_long_value(argv[1], &processes)) || processes < 0)
	{
	fprintf(stderr, USAGE, argv[0]);
	return 1;
	}


	//2nd arg: size of output buffer
	if (argc > 2 && argc < 4 && strlen(argv[2]) && (!(parse_long_value(argv[2], &line_size)) || line_size < 2))
	{
	fprintf(stderr, USAGE, argv[0]);
	return 1;
	}


	//array of socket buffers to communicate with forks
	usable_socket *all_sockets = calloc(processes, sizeof(usable_socket));
	if (!all_sockets)
	{
	fprintf(stderr, "%s: allocation error: %s\n", argv[0], strerror(errno));
	return 1;
	}

	//3rd+ args: a command to use in place of the default process executor
	char **command = (argc > 3)? (argv + 3) : NULL;


	setlinebuf(stderr);


	if (isatty(STDIN_FILENO))
	{
	fprintf(stderr, "%s: refusing to read commands directly from the terminal\n", argv[0]);
	return 1;
	}


	//create the specified number of monitoring processes

	for (I = 0; I < processes; I++)
	{
	//socket pair for 2-way communication
	int new_sockets[2] = { -1, -1 }, J;
	if (socketpair(PF_LOCAL, SOCK_STREAM, 0, new_sockets) < 0)
	 {
	fprintf(stderr, "%s: socket error: %s\n", argv[0], strerror(errno));
	break;
	 }

	//close the sockets when executing a new process
	fcntl(new_sockets[0], F_SETFD, fcntl(new_sockets[0], F_GETFD) | FD_CLOEXEC);
	fcntl(new_sockets[1], F_SETFD, fcntl(new_sockets[1], F_GETFD) | FD_CLOEXEC);

	//new process
	pid_t next_process = fork();


	//fork error
	if (next_process < 0)
	 {
	fprintf(stderr, "%s: fork error: %s\n", argv[0], strerror(errno));
	close(new_sockets[0]);
	close(new_sockets[1]);
	break;
	 }


	//forked process
	else if (next_process == 0)
	 {
	register_handlers(0);

	//manually close the socket buffers used by the master process
	for (J = 0; J < processes; J++)
	if (all_sockets[J].file) fclose(all_sockets[J].file);

	free(all_sockets);
	close(new_sockets[0]);

	//stop until further notice
	raise(SIGSTOP);


	//execute a custom command for looping through commands
	if (command)
	  {
	//leave this end of the socket with the master process open when executing the command
	fcntl(new_sockets[1], F_SETFD, fcntl(new_sockets[1], F_GETFD) & ~FD_CLOEXEC);

	int line = new_sockets[1], ready = dup(line), null_in = open("/dev/null", O_RDONLY);

	dup2(null_in, STDIN_FILENO);
	close(null_in);

	//NOTE: need separate strings here!
	char id_buffer[256]     = { 0x00 }, size_buffer[256]    = { 0x00 },
	     parallel_line[256] = { 0x00 }, parallel_ready[256] = { 0x00 };

	snprintf(id_buffer, sizeof id_buffer, "PARALLEL_EXEC_ID=%i", (int) I + 1);
	putenv(id_buffer);

	//NOTE: directly copy the string
	snprintf(size_buffer, sizeof size_buffer, "PARALLEL_EXEC_BUFFER=%s", argv[2]);
	putenv(size_buffer);

	snprintf(parallel_line, sizeof parallel_line, "PARALLEL_EXEC_LINE=%i", line);
	putenv(parallel_line);

	snprintf(parallel_ready, sizeof parallel_ready, "PARALLEL_EXEC_READY=%i", ready);
	putenv(parallel_ready);

	//execute the command
	execvp(command[0], command);
	fprintf(stderr, "%s: exec error: %s\n", argv[0], strerror(errno));
	_exit(0);
	  }


	//use the default function for looping through commands
	else return process_loop(argv[0], new_sockets[1], I + 1, line_size);
	 }


	//original process
	else
	 {
	status = 0;

	//wait for the newest fork to stop
	while (waitpid(next_process, &status, WUNTRACED) == 0 && !WIFSTOPPED(status));
	if (!WIFSTOPPED(status))
	  {
	fprintf(stderr, "%s: error synchronizing with fork: %s\n", argv[0], strerror(errno));
	close(new_sockets[0]);
	close(new_sockets[1]);
	kill(next_process, SIGTERM);
	return 1;
	  }

	//set the fork's process group
	setpgid(next_process, getpgid(0));

	//create a new buffer
	if (!(all_sockets[I].file = fdopen(new_sockets[0], "a+")))
	  {
	fprintf(stderr, "%s: stream error: %s\n", argv[0], strerror(errno));
	close(new_sockets[0]);
	close(new_sockets[1]);
	kill(next_process, SIGCONT);
	continue;
	  }

	all_sockets[I].socket = new_sockets[0];
	close(new_sockets[1]);
	 }
	}


	//register signal handlers, then continue all of the forks
	register_handlers(1);
	killpg(0, SIGCONT);


	//loop through the shell commands and wait for forks to exit
	int outcome = select_loop(argv[0], all_sockets, (unsigned) processes);
	while (wait(&status) >= 0 || errno == EINTR) outcome |= status;

	return outcome;
}


static int parse_long_value(const char *dData, int *vValue)
{
	if (!dData || !*dData || !vValue) return 0;
	char *endpoint = NULL;
	*vValue = strtol(dData, &endpoint, 10);
	return endpoint && (*endpoint == 0x00);
}


static int execute(const char *nName, unsigned int nNumber, const char *cCommand, char *bBuffer, unsigned int sSize)
{
	int send_output = -1, recv_output = -1;

	if (bBuffer)
	{
	int pipes[2];
	if (pipe(pipes) != 0)
	 {
	fprintf(stderr, "%s[%u]: unable to create output pipe: %s\n", nName, nNumber, strerror(errno));
	 }

	else
	 {
	send_output = pipes[0];
	recv_output = pipes[1];
	 }
	}

	pid_t new_command = fork();

	//fork error
	if (new_command < 0)
	{
	fprintf(stderr, "%s[%u]: fork error: %s\n", nName, nNumber, strerror(errno));
	if (send_output >= 0) close(send_output);
	if (recv_output >= 0) close(recv_output);
	return new_command;
	}


	//forked process
	if (new_command == 0)
	{
	//check for '$SHELL'
	char *shell = getenv("SHELL");

	//check the current user's default shell
	if (!shell || !strlen(shell))
	 {
	struct passwd *user = getpwuid(getuid());
	if (user) shell = user->pw_shell;
	endpwent();
	 }

	char *argv[] = { strdup((shell && strlen(shell))? shell : "/bin/sh"), strdup("-c"), strdup(cCommand), NULL };

	if (send_output > 0)
	 {
	if (dup2(send_output, STDOUT_FILENO) == -1)
	  {
	fprintf(stderr, "%s[%u]: unable to duplicate output pipe: %s\n", nName, nNumber, strerror(errno));
	close(send_output);
	send_output = -1;
	  }

	else close(send_output);
	 }

	if (recv_output >= 0) close(recv_output);

	//execute the shell command
	execvp(argv[0], argv);
	fprintf(stderr, "%s[%u]: exec error: %s\n", nName, nNumber, strerror(errno));
	_exit(0);
	}


	//original process
	else
	{
	if (send_output >= 0) close(send_output);

	if (recv_output >= 0)
	 {
	FILE *output = fdopen(recv_output, "r");
	if (!output)
	  {
	fprintf(stderr, "%s[%u]: unable to create output file: %s\n", nName, nNumber, strerror(errno));
	close(recv_output);
	send_output = -1;
	  }


	//buffer the command's output (single lines only)

	int lockable = 1;

	while (fgets(bBuffer, sSize, output))
	  {
	if (lockable)
	   {
	struct flock set_lock = { .l_type = F_WRLCK, .l_whence = SEEK_CUR };
	if (fcntl(STDOUT_FILENO, F_SETLKW, &set_lock, NULL) == -1) lockable = 0;
	if (!lockable) fprintf(stderr, "%s[%u]: unable to lock output file: %s\n", nName, nNumber, strerror(errno));
	   }

	fprintf(stdout, "%s", bBuffer);
	fflush(stdout);

	if (lockable)
	   {
	struct flock set_lock = { .l_type = F_UNLCK, .l_whence = SEEK_CUR };
	if(fcntl(STDOUT_FILENO, F_SETLKW, &set_lock, NULL) == -1) lockable = 0;
	if (!lockable) fprintf(stderr, "%s[%u]: unable to unlock output file: %s\n", nName, nNumber, strerror(errno));
	   }
	  }
	 }


	//wait for the command to complete
	int status;
	while (waitpid(new_command, &status, 0x00) == 0 && !WIFEXITED(status));
	return WEXITSTATUS(status);
	}
}


static int process_loop(const char *nName, int sSocket, unsigned int nNumber, unsigned int lLine)
{
	int returned = 0;

	FILE *parent_file = fdopen(sSocket, "a+");
	if (!parent_file)
	{
	fprintf(stderr, "%s[%u]: stream error: %s\n", nName, nNumber, strerror(errno));
	close(sSocket);
	return 1;
	}

	const int buffer_size = BUFFER_PAGES * sysconf(_SC_PAGESIZE);

	char *buffer = malloc(buffer_size);
	if (!buffer)
	{
	fprintf(stderr, "%s[%u]: allocation error: %s\n", nName, nNumber, strerror(errno));
	fclose(parent_file);
	return 1;
	}

	char *line_buffer = lLine? malloc(lLine) : NULL;
	if (lLine && !line_buffer)
	{
	fprintf(stderr, "%s[%u]: allocation error: %s\n", nName, nNumber, strerror(errno));
	free(buffer);
	fclose(parent_file);
	return 1;
	}


	//read lines from standard input and execute them
	while (fprintf(parent_file, "%i\n", returned) && fgets(buffer, buffer_size, parent_file))
	{
	fprintf(stderr, "%s[%u]: executing: %s", nName, nNumber, buffer);
	fflush(stderr);
	returned = execute(nName, nNumber, buffer, line_buffer, lLine);
	}

	free(buffer);
	free(line_buffer);
	return 0;
}


static int select_loop(const char *nName, usable_socket *sSockets, unsigned int cCount)
{
	if (!sSockets) return 1;

	int I;
	const int buffer_size = BUFFER_PAGES * sysconf(_SC_PAGESIZE);
	char return_buffer[32];

	char *buffer = malloc(buffer_size);
	if (!buffer)
	{
	fprintf(stderr, "%s: allocation error: %s\n", nName, strerror(errno));
	return 1;
	}


	//read lines from standard input and pass them to the monitoring processes
	while (fgets(buffer, buffer_size, stdin))
	{
	int added = 0, I;

	//create a new list of sockets to select from
	//NOTE: this allows for a monitoring process to die

	fd_set input_sockets;
	FD_ZERO(&input_sockets);

	for (I = 0; I < cCount; I++)
	if (sSockets[I].file)
	 {
	FD_SET(sSockets[I].socket, &input_sockets);
	++added;
	 }

	if (!added) break;

	if (select(FD_SETSIZE, &input_sockets, NULL, NULL, NULL) >= 0 || errno == EBADF)
	for (I = 0; I < cCount; I++)
	if (sSockets[I].file && FD_ISSET(sSockets[I].socket, &input_sockets))
	 {
	if (errno == EBADF)
	  {
	fclose(sSockets[I].file);
	sSockets[I].socket = 0;
	sSockets[I].file   = NULL;
	  }

	//if the monitoring process is ready, send it the command
	//NOTE: if input is recieved without a newline, this could hang until a newline is read!
	//TODO: fix the thing mentioned above
	else if (fgets(return_buffer, sizeof return_buffer, sSockets[I].file))
	  {
	//TODO: do something with the return?
	fprintf(sSockets[I].file, "%s", buffer);
	if (fflush(sSockets[I].file) != 0)
	   {
	fclose(sSockets[I].file);
	sSockets[I].socket = 0;
	sSockets[I].file   = NULL;
	   }
	break;
	  }
	 }
	}

	for (I = 0; I < cCount; I++)
	if (sSockets[I].file) fclose(sSockets[I].file);

	free(buffer);
	free(sSockets);
	return 0;
}


static void exit_handler(int sSignal)
{
	signal(sSignal, SIG_DFL);
	killpg(0, sSignal);
}


static void register_handlers(int master)
{
    #ifdef SIGFPE
	signal(SIGFPE, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGILL
	signal(SIGILL, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGSEGV
	signal(SIGSEGV, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGBUS
	signal(SIGBUS, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGABRT
	signal(SIGABRT, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGIOT
	signal(SIGIOT, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGTRAP
	signal(SIGTRAP, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGEMT
	signal(SIGEMT, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGSYS
	signal(SIGSYS, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
    #endif

    #ifdef SIGLOST
	signal(SIGLOST, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGXCPU
	signal(SIGXCPU, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGXFSZ
	signal(SIGXFSZ, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGTERM
	signal(SIGTERM, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGINT
	signal(SIGINT, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGQUIT
	signal(SIGQUIT, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGHUP
	signal(SIGHUP, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGALRM
	signal(SIGALRM, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGVTALRM
	signal(SIGVTALRM, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGPROF
	signal(SIGPROF, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGTSTP
	signal(SIGTSTP, SIG_DFL);
    #endif

    #ifdef SIGTTIN
	signal(SIGTTIN, SIG_IGN);
    #endif

    #ifdef SIGTTOU
	signal(SIGTTOU, SIG_IGN);
    #endif

    #ifdef SIGUSR1
	signal(SIGUSR1, SIG_IGN);
    #endif

    #ifdef SIGUSR2
	signal(SIGUSR2, SIG_IGN);
    #endif

    #ifdef SIGPOLL
	signal(SIGPOLL, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGSTKFLT
	signal(SIGSTKFLT, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGIO
	signal(SIGIO, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGPWR
	signal(SIGPWR, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGINFO
	signal(SIGINFO, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGUNUSED
	signal(SIGUNUSED, master? &exit_handler : SIG_DFL);
    #endif

    #ifdef SIGSYS
	signal(SIGSYS, master? &exit_handler : SIG_DFL);
    #endif
}
