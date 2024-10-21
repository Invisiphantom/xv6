

struct stat;

// 系统调用接口 (usys.S)
int fork();
int exit(int status) __attribute__((noreturn));
int wait(int* status);
int pipe(int p[]);
int read(int fd, void* buf, int n);
int kill(int pid);
int exec(char* file, char* argv[]);
int fstat(int fd, struct stat* st);
int chdir(const char* dir);
int dup(int fd);
int getpid();
char* sbrk(int n);
int sleep(int n);
int uptime();
int open(const char* file, int flags);
int write(int fd, const char* buf, int n);
int mknod(const char* file, int mode, int dev);
int unlink(const char* file);
int link(const char* old, const char* new);
int mkdir(const char* dir);
int close(int fd);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void* memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...) __attribute__((format(printf, 2, 3)));
void printf(const char*, ...) __attribute__((format(printf, 1, 2)));
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
int atoi(const char*);
int memcmp(const void*, const void*, uint);
void* memcpy(void*, const void*, uint);

// umalloc.c
void* malloc(uint);
void free(void*);
