/* A positive control for tools/c1vm-test.sh's strace analysis: a program that does exactly what the VM runner must NOT do (open a socket and connect, write a file it was
   not given, remove a file, start a program). If the analysis does not flag this one, the tracer or the analysis is blind and the "0 found" it reports for the runner means
   nothing. Usage: strace_control <path it may create and remove> */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(9)};
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (s >= 0) { connect(s, (struct sockaddr *)&a, sizeof a); close(s); }
    int fd = open(argv[1], O_RDWR | O_CREAT, 0600);
    if (fd >= 0) close(fd);
    unlink(argv[1]);
    execl("/bin/true", "true", (char *)0);
    return 0;
}
