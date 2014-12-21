The parallel-exec program is a Linux/FreeBSD command-line utility for executing
multiple command-lines in parallel. To install:

  make && sudo make install


***** Overview *****

Suppose you want to execute a long-running script/program a large number of 
times. For example:

  - You have a script that preprocesses a single file, perhaps to extract and 
    transform some data.
  - You have a simulation program that you want to execute an arbitrary number 
    of times.

The parallel-exec program is designed to execute such repeated commands, keeping 
a preset number of commands running at one time. For example, you might want to 
execute your program 100 times, having 8 instances running at any given time. 
This is helpful for a few reasons:

  - You only need to run one command to make sure all 100 commands are executed.
  - You don't have to worry about having all 100 commands running at once.
  - If you want to terminate the entire process, you only need to terminate the 
    parallel-exec process overseeing the whole thing.


***** Basic Usage *****

The parallel-exec program has an extremely simple command-line interface (CLI). 
It has fixed-position arguments, and it doesn't take flags or options. It reads 
lines of input from standard input (usually a pipe) and executes each line of 
input as if it was typed into the shell. The format of a call is as follows:

  parallel-exec [process count] (buffer size) (command line...)

The arguments above have the following meanings:

    [process count]: The number of processes (i.e., command instances) to run at 
                     a time. This is mandatory!

    (buffer size): The number of bytes to buffer at a time when output is 
                   buffered (described below.) This is optional.

    (command line...): One or more arguments that form a call to another 
                       program, which will be used to manage the commands to be 
                       executed (described below.) This is optional; however, 
                       you must specify (buffer size) (even if it's blank) in 
                       order to use a custom command here.

You'll only need to specify the first argument ([process count]) in the vast 
majority of cases. Since the parallel-exec program takes fixed-position 
arguments (to reduce programming complexity), the first argument will always be 
treated as a process count, the second argument will always be taken as a buffer 
size, and the third+ arguments will always be taken (collectively) as a call to 
a program that's going to manage command execution.


----- Step 1: Generate Commands -----

The first step is to generate a list of shell commands, with exactly one command 
per line. This is most-easily done with a loop. Example: Suppose you want to 
call the script ./process-data.r on log files named datafile-*.txt:

  for datafile in datafile-*.txt; do
    echo "./process-data.r $datafile"
  done

For use on the command-line (vs. in a script), this can be written as a single 
line, as follows:

  for datafile in datafile-*.txt; do echo "./process-data.r $datafile"; done

This command will output a list that looks something like (assuming files 1-8):

  ./process-data.r datafile-1.txt
  ./process-data.r datafile-2.txt
  ./process-data.r datafile-3.txt
  ./process-data.r datafile-4.txt
  ./process-data.r datafile-5.txt
  ./process-data.r datafile-6.txt
  ./process-data.r datafile-7.txt
  ./process-data.r datafile-8.txt

Note that the purpose of this step is to output the commands in text format! The 
loop shouldn't do anything other than display the commands.


----- Step 2: Pipe Commands to parallel-exec -----

The second step in the process is to pipe the list of commands to the 
parallel-exec program. This requires that you first choose how many commands 
should be executed at one time. In the example below, 2 commands will be running 
at any given time:

  for datafile in datafile-*.txt; do echo "./process-data.r $datafile"; done | \
    parallel-exec 2

Unless your script/program prints lines that you'll want to look at later, this 
is literally all you need to do!


----- Step 3: Managing Output (Optional) -----

The output of the commands will be jumbled together unless you redirect the 
outputs of the individual commands. For example, suppose you want to log the 
error output of ./process-data.r separately for each instance:

  for datafile in datafile-*.txt; do echo \
    "./process-data.r $datafile 2> $datafile.log"; done

This will give you:

  ./process-data.r datafile-1.txt 2> datafile-1.txt.log
  ./process-data.r datafile-2.txt 2> datafile-2.txt.log
  ./process-data.r datafile-3.txt 2> datafile-3.txt.log
  ./process-data.r datafile-4.txt 2> datafile-4.txt.log
  ./process-data.r datafile-5.txt 2> datafile-5.txt.log
  ./process-data.r datafile-6.txt 2> datafile-6.txt.log
  ./process-data.r datafile-7.txt 2> datafile-7.txt.log
  ./process-data.r datafile-8.txt 2> datafile-8.txt.log

When parallel-exec executes these commands, standard error for each of the 
commands will go to a text file specific to that command instance. In a lot of 
cases this might not be necessary.

Alternatively, you can discard/redirect the output of all of the commands at 
once by redirecting the output of parallel-exec:

  - Discard error output:

    for datafile in datafile-*.txt; do echo "./process-data.r $datafile"; done \
      | parallel-exec 2 2> /dev/null

  - Capture standard output to a file:

    for datafile in datafile-*.txt; do echo "./process-data.r $datafile"; done \
      | parallel-exec > output.txt


***** Buffering Output *****

If for some reason your commands will be sending line-based output that needs to 
be used for something else to standard output, you can line-buffer the output of 
parallel-exec to keep the lines from printing over each other. For example, you
might have a script that generates one line of data at a time, and it doesn't
matter what order those lines occur in the output file.

To set a line-buffer size (e.g., 1024 bytes), add a second argument to the 
parallel-exec call:

  for datafile in datafile-*.txt; do echo "./process-data.r $datafile"; done | \
    parallel-exec 2 1024 > output.txt

This will do two things:

  - parallel-exec will capture lines of output from the executed commands (up to 
    1024 bytes long).
  - It will attempt to lock the output file (output.txt) while printing each 
    line, so that the printing of lines from other commands is delayed until the 
    current line is printed.


***** Using a Custom Looping Command *****

You might be in a situation where you don't want to unconditionally execute 
every command that's passed to parallel-exec, or you might want to do further 
interpretation of each line that's piped to parallel-exec. To use a custom 
looping command, pass additional arguments to parallel-exec (starting with 
argument 3). There are certain requirements of such a command, however.


----- Writing a Custom Script -----

To receive a line to execute, the custom looping command must write a line to 
the file descriptor indicated by the $PARALLEL_EXEC_READY environment variable, 
and it must read the line to be executed from the descriptor indicated by the 
$PARALLEL_EXEC_LINE environment variable. In bash, this can be done with:

  echo >&$PARALLEL_EXEC_READY && read line <&$PARALLEL_EXEC_LINE

This will store the line to be executed in the $line environment variable. 
Optionally, you can suppress the "Broken pipe" message that occurs when no more 
commands are left by redirecting standard error when writing to 
$PARALLEL_EXEC_READY:

  echo >&$PARALLEL_EXEC_READY 2> /dev/null && read line <&$PARALLEL_EXEC_LINE

To put this into a script (call it process.sh) with a loop:

  #!/usr/bin/env bash

  while echo >&$PARALLEL_EXEC_READY 2> /dev/null && \
    read line <&$PARALLEL_EXEC_LINE; do
    echo "$0[$PARALLEL_EXEC_ID]: I am going to execute the command" \
      "'$line' right now" 1>&2
    eval $line #<-- this executes the command!
  done

The script above makes use of $PARALLEL_EXEC_ID, which is the instance number of 
the script. If you call parallel-exec with n processes, the instances will be 
numbered from 1 through n (0 is reserved for the master process.)

You would use this script as follows:

for datafile in datafile-*.txt; do echo "./process-data.r $datafile"; done | \
  parallel-exec 2 '' ./process.sh

Note that the second argument (the output buffer size) is required, and is 
recorded in the $PARALLEL_EXEC_BUFFER environment variable, which is available 
from process.sh.


----- Example of an Advanced Custom Script -----

Suppose you want to go even more advanced, and dictate the CPU core that each 
process will run on. On Linux, you can use taskset -c # and on FreeBSD you can 
use cpuset -l #, where # is the CPU number, starting at 0:

  #!/usr/bin/env bash

  cpu=$((PARALLEL_EXEC_ID-1))

  while echo >&$PARALLEL_EXEC_READY 2> /dev/null && \
    read line <&$PARALLEL_EXEC_LINE; do
    echo "$0[$PARALLEL_EXEC_ID]: I am going to execute the command" \
      "'$line' on CPU $cpu" 1>&2

    ( #<-- creates a subprocess so we can close the parallel-exec descriptors)
      #(close the sockets that connect this script to parallel-exec)
      eval exec "$PARALLEL_EXEC_LINE<&-"
      unset PARALLEL_EXEC_LINE
      eval exec "$PARALLEL_EXEC_READY>&-"
      unset PARALLEL_EXEC_READY

      eval taskset -c $cpu $line
    )
  done


***** THE END *****

Kevin P. Barry [ta0kira@gmail.com], 20141221
