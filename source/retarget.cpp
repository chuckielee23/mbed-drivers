/* mbed Microcontroller Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "platform.h"
#include "FileHandle.h"
#include "FileSystemLike.h"
#include "FilePath.h"
#include "serial_api.h"
#include "toolchain.h"
#include "cmsis.h"
#include <errno.h>
#include "app.h"
#include "minar/minar.h"

#if defined(__ARMCC_VERSION)
#   include <rt_sys.h>
#   define PREFIX(x)    _sys##x
#   define OPEN_MAX     _SYS_OPEN
#   ifdef __MICROLIB
#       pragma import(__use_full_stdio)
#   endif

#elif defined(__ICCARM__)
#   include <yfuns.h>
#   define PREFIX(x)        _##x
#   define OPEN_MAX         16

#   define STDIN_FILENO     0
#   define STDOUT_FILENO    1
#   define STDERR_FILENO    2

#else
#   include <sys/stat.h>
#   include <sys/unistd.h>
#   include <sys/syslimits.h>
#   define PREFIX(x)    x
#endif

#ifndef pid_t
 typedef int pid_t;
#endif
#ifndef caddr_t
 typedef char * caddr_t;
#endif

using namespace mbed;

#if defined(__MICROLIB) && (__ARMCC_VERSION>5030000)
// Before version 5.03, we were using a patched version of microlib with proper names
extern const char __stdin_name[]  = ":tt";
extern const char __stdout_name[] = ":tt";
extern const char __stderr_name[] = ":tt";

#else
extern const char __stdin_name[]  = "/stdin";
extern const char __stdout_name[] = "/stdout";
extern const char __stderr_name[] = "/stderr";
#endif

/* newlib has the filehandle field in the FILE struct as a short, so
 * we can't just return a Filehandle* from _open and instead have to
 * put it in a filehandles array and return the index into that array
 * (or rather index+3, as filehandles 0-2 are stdin/out/err).
 */
static FileHandle *filehandles[OPEN_MAX];

FileHandle::~FileHandle() {
    /* Remove all open filehandles for this */
    for (unsigned int fh_i = 0; fh_i < sizeof(filehandles)/sizeof(*filehandles); fh_i++) {
        if (filehandles[fh_i] == this) {
            filehandles[fh_i] = NULL;
        }
    }
}

#if DEVICE_SERIAL
extern int stdio_uart_inited;
extern serial_t stdio_uart;
#endif

static void init_serial() {
#if DEVICE_SERIAL
    if (stdio_uart_inited) return;
    serial_init(&stdio_uart, STDIO_UART_TX, STDIO_UART_RX);
#endif
}

static inline int openmode_to_posix(int openmode) {
    int posix = openmode;
#ifdef __ARMCC_VERSION
    if (openmode & OPEN_PLUS) {
        posix = O_RDWR;
    } else if(openmode & OPEN_W) {
        posix = O_WRONLY;
    } else if(openmode & OPEN_A) {
        posix = O_WRONLY|O_APPEND;
    } else {
        posix = O_RDONLY;
    }
    /* a, w, a+, w+ all create if file does not already exist */
    if (openmode & (OPEN_A|OPEN_W)) {
        posix |= O_CREAT;
    }
    /* w and w+ truncate */
    if (openmode & OPEN_W) {
        posix |= O_TRUNC;
    }
#elif defined(__ICCARM__)
    switch (openmode & _LLIO_RDWRMASK) {
        case _LLIO_RDONLY: posix = O_RDONLY; break;
        case _LLIO_WRONLY: posix = O_WRONLY; break;
        case _LLIO_RDWR  : posix = O_RDWR  ; break;
    }
    if (openmode & _LLIO_CREAT ) posix |= O_CREAT;
    if (openmode & _LLIO_APPEND) posix |= O_APPEND;
    if (openmode & _LLIO_TRUNC ) posix |= O_TRUNC;
#endif
    return posix;
}

extern "C" FILEHANDLE PREFIX(_open)(const char* name, int openmode) {
    #if defined(__MICROLIB) && (__ARMCC_VERSION>5030000)
    // Before version 5.03, we were using a patched version of microlib with proper names
    // This is the workaround that the microlib author suggested us
    static int n = 0;
    if (!std::strcmp(name, ":tt")) return n++;

    #else
    /* Use the posix convention that stdin,out,err are filehandles 0,1,2.
     */
    if (std::strcmp(name, __stdin_name) == 0) {
        init_serial();
        return 0;
    } else if (std::strcmp(name, __stdout_name) == 0) {
        init_serial();
        return 1;
    } else if (std::strcmp(name, __stderr_name) == 0) {
        init_serial();
        return 2;
    }
    #endif

    // find the first empty slot in filehandles
    unsigned int fh_i;
    for (fh_i = 0; fh_i < sizeof(filehandles)/sizeof(*filehandles); fh_i++) {
        if (filehandles[fh_i] == NULL) break;
    }
    if (fh_i >= sizeof(filehandles)/sizeof(*filehandles)) {
        return -1;
    }

    FileHandle *res;

    /* FILENAME: ":0x12345678" describes a FileLike* */
    if (name[0] == ':') {
        void *p;
        sscanf(name, ":%p", &p);
        res = (FileHandle*)p;

    /* FILENAME: "/file_system/file_name" */
    } else {
        FilePath path(name);

        if (!path.exists())
            return -1;
        else if (path.isFile()) {
            res = path.file();
        } else {
            FileSystemLike *fs = path.fileSystem();
            if (fs == NULL) return -1;
            int posix_mode = openmode_to_posix(openmode);
            res = fs->open(path.fileName(), posix_mode); /* NULL if fails */
        }
    }

    if (res == NULL) return -1;
    filehandles[fh_i] = res;

    return fh_i + 3; // +3 as filehandles 0-2 are stdin/out/err
}

extern "C" int PREFIX(_close)(FILEHANDLE fh) {
    if (fh < 3) return 0;

    FileHandle* fhc = filehandles[fh-3];
    filehandles[fh-3] = NULL;
    if (fhc == NULL) return -1;

    return fhc->close();
}

#if defined(__ICCARM__)
extern "C" size_t    __write (int        fh, const unsigned char *buffer, size_t length) {
#else
extern "C" int PREFIX(_write)(FILEHANDLE fh, const unsigned char *buffer, unsigned int length, int mode) {
#endif
    (void) mode;
    int n; // n is the number of bytes written
    if (fh < 3) {
#if DEVICE_SERIAL
        if (!stdio_uart_inited) init_serial();
        for (unsigned int i = 0; i < length; i++) {
            serial_putc(&stdio_uart, buffer[i]);
        }
#endif
        n = length;
    } else {
        FileHandle* fhc = filehandles[fh-3];
        if (fhc == NULL) return -1;

        n = fhc->write(buffer, length);
    }
#ifdef __ARMCC_VERSION
    return length-n;
#else
    return n;
#endif
}

#if defined(__ICCARM__)
extern "C" size_t    __read (int        fh, unsigned char *buffer, size_t       length) {
#else
extern "C" int PREFIX(_read)(FILEHANDLE fh, unsigned char *buffer, unsigned int length, int mode) {
#endif
    (void) mode;
    int n; // n is the number of bytes read
    if (fh < 3) {
        // only read a character at a time from stdin
#if DEVICE_SERIAL
        if (!stdio_uart_inited) init_serial();
        *buffer = serial_getc(&stdio_uart);
#endif
        n = 1;
    } else {
        FileHandle* fhc = filehandles[fh-3];
        if (fhc == NULL) return -1;

        n = fhc->read(buffer, length);
    }
#ifdef __ARMCC_VERSION
    return length-n;
#else
    return n;
#endif
}

#ifdef __ARMCC_VERSION
extern "C" int PREFIX(_istty)(FILEHANDLE fh)
#else
extern "C" int _isatty(FILEHANDLE fh)
#endif
{
    /* stdin, stdout and stderr should be tty */
    if (fh < 3) return 1;

    FileHandle* fhc = filehandles[fh-3];
    if (fhc == NULL) return -1;

    return fhc->isatty();
}

extern "C"
#if defined(__ARMCC_VERSION)
int _sys_seek(FILEHANDLE fh, long position)
#elif defined(__ICCARM__)
long __lseek(int fh, long offset, int whence)
#else
int _lseek(FILEHANDLE fh, int offset, int whence)
#endif
{
    if (fh < 3) return 0;

    FileHandle* fhc = filehandles[fh-3];
    if (fhc == NULL) return -1;

#if defined(__ARMCC_VERSION)
    return fhc->lseek(position, SEEK_SET);
#else
    return fhc->lseek(offset, whence);
#endif
}

#ifdef __ARMCC_VERSION
extern "C" int PREFIX(_ensure)(FILEHANDLE fh) {
    if (fh < 3) return 0;

    FileHandle* fhc = filehandles[fh-3];
    if (fhc == NULL) return -1;

    return fhc->fsync();
}

extern "C" long PREFIX(_flen)(FILEHANDLE fh) {
    if (fh < 3) return 0;

    FileHandle* fhc = filehandles[fh-3];
    if (fhc == NULL) return -1;

    return fhc->flen();
}
#endif


#if !defined(__ARMCC_VERSION) && !defined(__ICCARM__)
extern "C" int _fstat(int fd, struct stat *st) {
    if ((STDOUT_FILENO == fd) || (STDERR_FILENO == fd) || (STDIN_FILENO == fd)) {
        st->st_mode = S_IFCHR;
        return  0;
    }

    errno = EBADF;
    return -1;
}
#endif

namespace std {
extern "C" int remove(const char *path) {
    FilePath fp(path);
    FileSystemLike *fs = fp.fileSystem();
    if (fs == NULL) return -1;

    return fs->remove(fp.fileName());
}

extern "C" int rename(const char *oldname, const char *newname) {
    FilePath fpOld(oldname);
    FilePath fpNew(newname);
    FileSystemLike *fsOld = fpOld.fileSystem();
    FileSystemLike *fsNew = fpNew.fileSystem();

    /* rename only if both files are on the same FS */
    if (fsOld != fsNew || fsOld == NULL) return -1;

    return fsOld->rename(fpOld.fileName(), fpNew.fileName());
}

extern "C" char *tmpnam(char *s) {
    (void) s;
    return NULL;
}

extern "C" FILE *tmpfile() {
    return NULL;
}
} // namespace std

#ifdef __ARMCC_VERSION
extern "C" char *_sys_command_string(char *cmd, int len) {
    return NULL;
}
#endif

extern "C" DIR *opendir(const char *path) {
    /* root dir is FileSystemLike */
    if (path[0] == '/' && path[1] == 0) {
        return FileSystemLike::opendir();
    }

    FilePath fp(path);
    FileSystemLike* fs = fp.fileSystem();
    if (fs == NULL) return NULL;

    return fs->opendir(fp.fileName());
}

extern "C" struct dirent *readdir(DIR *dir) {
    return dir->readdir();
}

extern "C" int closedir(DIR *dir) {
    return dir->closedir();
}

extern "C" void rewinddir(DIR *dir) {
    dir->rewinddir();
}

extern "C" off_t telldir(DIR *dir) {
    return dir->telldir();
}

extern "C" void seekdir(DIR *dir, off_t off) {
    dir->seekdir(off);
}

extern "C" int mkdir(const char *path, mode_t mode) {
    FilePath fp(path);
    FileSystemLike *fs = fp.fileSystem();
    if (fs == NULL) return -1;

    return fs->mkdir(fp.fileName(), mode);
}

#if defined(TOOLCHAIN_GCC) || defined(TARGET_LIKE_CLANG)
/* prevents the exception handling name demangling code getting pulled in */
#include "mbed_error.h"
namespace __gnu_cxx {
    void __verbose_terminate_handler() {
        error("Exception");
    }
}
extern "C" WEAK void __cxa_pure_virtual(void);
extern "C" WEAK void __cxa_pure_virtual(void) {
    exit(1);
}

#endif

// ****************************************************************************
// mbed_sdk_init() is a function that is called before starting the scheduler,
// it is not meant for user code, but for HAL ports to perform initialization
// before the scheduler is started

extern "C" WEAK void mbed_sdk_init(void);
extern "C" WEAK void mbed_sdk_init(void) {
}

#if defined(TOOLCHAIN_ARM)
extern "C" int $Super$$main(void);

extern "C" int $Sub$$main(void) {
    mbed_sdk_init();
    return $Super$$main();
}
#elif defined(TOOLCHAIN_GCC)  || defined(TARGET_LIKE_CLANG)
extern "C" int __real_main(void);

extern "C" int __wrap_main(void) {
    mbed_sdk_init();
    return __real_main();
}
#elif defined(TOOLCHAIN_IAR)
// IAR doesn't have the $Super/$Sub mechanism of armcc, nor something equivalent
// to ld's --wrap. It does have a --redirect, but that doesn't help, since redirecting
// 'main' to another symbol looses the original 'main' symbol. However, its startup
// code will call a function to setup argc and argv (__iar_argc_argv) if it is defined.
// Since mbed doesn't use argc/argv, we use this function to call mbed_sdk_init.
extern "C" void __iar_argc_argv() {
    mbed_sdk_init();
}
#endif

// the user should set up their application in app_start
extern "C" int main(void) {
    minar::Scheduler::postCallback(
        FunctionPointer2<void, int, char**>(&app_start).bind(0, NULL)
    );
    return minar::Scheduler::start();
}



extern "C" void _exit(int status)
{
    (void) status;
    while(1) {
        __BKPT(0);
    }
}
extern "C" int _kill(pid_t pid, int sig)
{
    (void) pid;
    (void) sig;
    errno = EINVAL;
    return -1;
}
extern "C" pid_t _getpid(void)
{
    return 0;
}

#include "mbed-util/atomic_ops.h"
#include "mbed/sbrk.h"

void * volatile mbed_krbs_ptr = &__mbed_krbs_start;
void * volatile mbed_sbrk_ptr = &__mbed_sbrk_start;
volatile ptrdiff_t mbed_sbrk_diff = MBED_HEAP_SIZE;

void * mbed_sbrk(ptrdiff_t size)
{
    if (size == 0) {
        return (void *) mbed_sbrk_ptr;
    }

    ptrdiff_t size_internal = size;
    // Minimum increment only applies to positive sbrks
    if (size_internal > 0) {
        if ((uintptr_t)size_internal < SBRK_INC_MIN) {
                size_internal = SBRK_INC_MIN;
        }
        size_internal = ( size_internal + SBRK_ALIGN - 1) & ~(SBRK_ALIGN - 1);
    }

    /* Decrement mbed_sbrk_diff by the size being allocated. */
    uint32_t ptr_diff = mbed_sbrk_diff;
    while (1) {
        if (size_internal > ptr_diff) {
            return (void *) -1;
        }
        if (mbed::util::atomic_cas((uint32_t *)&mbed_sbrk_diff, &ptr_diff, ptr_diff - size_internal)) {
            break;
        }
    }

    uintptr_t new_sbrk_ptr = mbed::util::atomic_incr((uint32_t *)&mbed_sbrk_ptr, (uint32_t)size_internal);
    return (void *)(new_sbrk_ptr - size_internal);
}

void * mbed_krbs(const ptrdiff_t size)
{
    return mbed_krbs_ex(size, NULL);
}

void * mbed_krbs_ex(const ptrdiff_t size, ptrdiff_t *actual)
{
    if (size == 0) {
        return (void *) mbed_krbs_ptr;
    }
    // krbs does not support deallocation.
    if (size < 0) {
        return (void *) -1;
    }

    uintptr_t size_internal = (uintptr_t) size;
    // Guarantee minimum allocation size
    if (size_internal < KRBS_INC_MIN) {
        size_internal = KRBS_INC_MIN;
    }
    size_internal = (size_internal + KRBS_ALIGN - 1) & ~(KRBS_ALIGN - 1);

    /* Decrement mbed_sbrk_diff by the size being allocated. */
    uint32_t ptr_diff = mbed_sbrk_diff;
    while (1) {
        if ((size_internal > (uintptr_t)ptr_diff) && (actual == NULL)) {
            return (void *) -1;
        }
        if (mbed::util::atomic_cas((uint32_t *)&mbed_sbrk_diff, &ptr_diff, ptr_diff - size_internal)) {
            break;
        }
    }

    return (void *)mbed::util::atomic_decr((uint32_t *)&mbed_krbs_ptr, (uint32_t)size_internal);
}
