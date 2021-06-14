/* Copyright (c) 1999 Guenter Geiger and others.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/*
 * This file implements the loader for linux, which includes
 * a little bit of path handling.
 *
 * Generalized by MSP to provide an open_via_path function
 * and lists of files for all purposes.
 */

/* #define DEBUG(x) x */
#define DEBUG(x)

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

#ifdef _WIN32
# include <malloc.h> /* MSVC or mingw on windows */
#elif defined(__linux__) || defined(__APPLE__)
# include <alloca.h> /* linux, mac, mingw, cygwin */
#else
# include <stdlib.h> /* BSDs for example */
#endif

#include <string.h>
#include "m_pd.h"
#include "m_imp.h"
#include "s_stuff.h"
#include "s_utf8.h"
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>

#ifdef _LARGEFILE64_SOURCE
# define open  open64
# define lseek lseek64
# define fstat fstat64
# define stat  stat64
#endif

#ifdef _MSC_VER
# define snprintf _snprintf
#endif


typedef struct _namedlist {
    t_symbol   *name;
    t_namelist *list;
    struct _namedlist *next;
} t_namedlist;
#define MAXPDLOCALESTRING 10
typedef struct _pathstuff {
    t_namedlist *ps_namedlists;
    char ps_lang[MAXPDLOCALESTRING];
    char ps_lang_region[MAXPDLOCALESTRING];
} t_pathstuff;
#define PATHSTUFF ((t_pathstuff*)(pd_this->pd_stuff->st_private))

    /* change '/' characters to the system's native file separator */
void sys_bashfilename(const char *from, char *to)
{
    char c;
    while ((c = *from++))
    {
#ifdef _WIN32
        if (c == '/') c = '\\';
#endif
        *to++ = c;
    }
    *to = 0;
}

    /* change the system's native file separator to '/' characters  */
void sys_unbashfilename(const char *from, char *to)
{
    char c;
    while ((c = *from++))
    {
#ifdef _WIN32
        if (c == '\\') c = '/';
#endif
        *to++ = c;
    }
    *to = 0;
}

/* test if path is absolute or relative, based on leading /, env vars, ~, etc */
int sys_isabsolutepath(const char *dir)
{
    if (dir[0] == '/' || dir[0] == '~'
#ifdef _WIN32
        || dir[0] == '%' || (dir[1] == ':' && dir[2] == '/')
#endif
        )
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

/* expand env vars and ~ at the beginning of a path and make a copy to return */
static void sys_expandpath(const char *from, char *to, int bufsize)
{
    if ((strlen(from) == 1 && from[0] == '~') || (strncmp(from,"~/", 2) == 0))
    {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
#else
        const char *home = getenv("HOME");
#endif
        if (home)
        {
            strncpy(to, home, bufsize);
            to[bufsize-1] = 0;
            strncpy(to + strlen(to), from + 1, bufsize - strlen(to));
            to[bufsize-1] = 0;
        }
        else *to = 0;
    }
    else
    {
        strncpy(to, from, bufsize);
        to[bufsize-1] = 0;
    }
#ifdef _WIN32
    {
        char *buf = alloca(bufsize);
        ExpandEnvironmentStrings(to, buf, bufsize-1);
        buf[bufsize-1] = 0;
        strncpy(to, buf, bufsize);
        to[bufsize-1] = 0;
    }
#endif
}

/*******************  Utility functions used below ******************/

/*!
 * \brief copy until delimiter
 *
 * \arg to destination buffer
 * \arg to_len destination buffer length
 * \arg from source buffer
 * \arg delim string delimiter to stop copying on
 *
 * \return position after delimiter in string.  If it was the last
 *         substring, return NULL.
 */
static const char *strtokcpy(char *to, size_t to_len, const char *from, char delim)
{
    unsigned int i = 0;

        for (; i < (to_len - 1) && from[i] && from[i] != delim; i++)
                to[i] = from[i];
        to[i] = '\0';

        if (i && from[i] != '\0')
                return from + i + 1;

        return NULL;
}

/* add a single item to a namelist.  If "allowdup" is true, duplicates
may be added; otherwise they're dropped.  */

t_namelist *namelist_append(t_namelist *listwas, const char *s, int allowdup)
{
    t_namelist *nl, *nl2;
    nl2 = (t_namelist *)(getbytes(sizeof(*nl)));
    nl2->nl_next = 0;
    nl2->nl_string = (char *)getbytes(strlen(s) + 1);
    strcpy(nl2->nl_string, s);
    sys_unbashfilename(nl2->nl_string, nl2->nl_string);
    if (!listwas)
        return (nl2);
    else
    {
        for (nl = listwas; ;)
        {
            if (!allowdup && !strcmp(nl->nl_string, s))
            {
                freebytes(nl2->nl_string, strlen(nl2->nl_string) + 1);
                return (listwas);
            }
            if (!nl->nl_next)
                break;
            nl = nl->nl_next;
        }
        nl->nl_next = nl2;
    }
    return (listwas);
}

/* add a colon-separated list of names to a namelist */

#ifdef _WIN32
#define SEPARATOR ';'   /* in MSW the natural separator is semicolon instead */
#else
#define SEPARATOR ':'
#endif

t_namelist *namelist_append_files(t_namelist *listwas, const char *s)
{
    const char *npos;
    char temp[MAXPDSTRING];
    t_namelist *nl = listwas;

    npos = s;
    do
    {
        npos = strtokcpy(temp, sizeof(temp), npos, SEPARATOR);
        if (! *temp) continue;
        nl = namelist_append(nl, temp, 0);
    }
        while (npos);
    return (nl);
}

void namelist_free(t_namelist *listwas)
{
    t_namelist *nl, *nl2;
    for (nl = listwas; nl; nl = nl2)
    {
        nl2 = nl->nl_next;
        t_freebytes(nl->nl_string, strlen(nl->nl_string) + 1);
        t_freebytes(nl, sizeof(*nl));
    }
}

const char *namelist_get(const t_namelist *namelist, int n)
{
    int i;
    const t_namelist *nl;
    for (i = 0, nl = namelist; i < n && nl; i++, nl = nl->nl_next)
        ;
    return (nl ? nl->nl_string : 0);
}


static
t_namelist **default_namedlist(const char*listname) {
#define DEFAULT_NAMEDLIST(name, member) \
    if(!strncmp(listname, name, MAXPDSTRING))  \
        return &(STUFF->member)
    DEFAULT_NAMEDLIST("searchpath.temp", st_temppath);
    DEFAULT_NAMEDLIST("searchpath.main", st_searchpath);
    DEFAULT_NAMEDLIST("searchpath.static", st_staticpath);
    DEFAULT_NAMEDLIST("helppath.main", st_helppath);
    return 0;
}
static
t_namelist **namedlist_do_getlist(const char*listname) {
    t_namedlist*namedlists=PATHSTUFF->ps_namedlists, *nl=0;
    t_namelist**defaultlist=default_namedlist(listname);
    t_symbol*listname_sym=0;
    if(defaultlist)
        return defaultlist;
    if(!listname)
        return 0;
    listname_sym = gensym(listname);
    for(nl=namedlists; nl; nl=nl->next) {
        if(nl->name == listname_sym)
            return &nl->list;
    }
    return 0;
}
t_namelist *namedlist_getlist(const char*listname) {
    t_namelist**nl=namedlist_do_getlist(listname);
    if(nl)
        return *nl;
    return 0;
}
        /* append a new name to the named list;
         * if 'name' is non-NULL it is added to the list (modulo allowdup)
         *+ and any non-existing list is created
         */
void namedlist_append(const char*listname, const char*name, int allowdup) {
        /* FIXXME: implement namedlist_append */
    t_namelist**namelistp=namedlist_do_getlist(listname);
    t_namelist*namelist=namelistp?*namelistp:0;
    if(!name)
        return;
    if(!listname)
        return;
    if(!namelistp) {
            /* list does not exist: create it */
        t_namedlist*nl = (t_namedlist *)(getbytes(sizeof(*nl)));
        if(!nl)
            return;
        nl->name = gensym(listname);
        nl->next = PATHSTUFF->ps_namedlists;
        PATHSTUFF->ps_namedlists = nl;
        namelist = nl->list;
        namelistp = &nl->list;
    }

        /* append the name to the namelist, possibly creating it */
    namelist = namelist_append(namelist, name, allowdup);
        /* and store the new list-address for later use */
    *namelistp = namelist;
    return;
}
void namedlist_append_files(const char *listname, const char *s) {
    const char *npos;
    char temp[MAXPDSTRING];

    npos = s;
    do {
        npos = strtokcpy(temp, sizeof(temp), npos, SEPARATOR);
        if (! *temp) continue;
        namedlist_append(listname, temp, 0);
    } while (npos);
    return namedlist_append(listname, 0, 0);
}

void namedlist_free(const char*listname) {
    t_namelist**defaultlist=default_namedlist(listname);
    t_namelist*namelist=defaultlist?*defaultlist:0;
    t_namedlist*namedlists=PATHSTUFF->ps_namedlists, *nl=0;
    if(!listname)
        return;
    if(!namelist) {
            /* not a default list: find the list of the given name (and create one if it doesn't exist) */
        t_symbol*listname_sym = gensym(listname);
        for(nl=namedlists; nl; nl=nl->next) {
            if(nl->name == listname_sym)
                break;
        }
            /* not freeing non-existant list */
        if(!nl) return;

        namelist = nl->list;
    }
    namelist_free(namelist);
    if(defaultlist)
        *defaultlist = 0;
    if(nl)
        nl->list = 0;
}


int sys_usestdpath = 1;

void sys_setextrapath(const char *p)
{
    char pathbuf[MAXPDSTRING];
    namelist_free(STUFF->st_staticpath);
    /* add standard place for users to install stuff first */
#ifdef __gnu_linux__
    sys_expandpath("~/.local/lib/pd/extra/", pathbuf, MAXPDSTRING);
    STUFF->st_staticpath = namelist_append(0, pathbuf, 0);
    sys_expandpath("~/pd-externals", pathbuf, MAXPDSTRING);
    STUFF->st_staticpath = namelist_append(STUFF->st_staticpath, pathbuf, 0);
    STUFF->st_staticpath = namelist_append(STUFF->st_staticpath,
        "/usr/local/lib/pd-externals", 0);
#endif

#ifdef __APPLE__
    sys_expandpath("~/Library/Pd", pathbuf, MAXPDSTRING);
    STUFF->st_staticpath = namelist_append(0, pathbuf, 0);
    STUFF->st_staticpath = namelist_append(STUFF->st_staticpath, "/Library/Pd", 0);
#endif

#ifdef _WIN32
    sys_expandpath("%AppData%/Pd", pathbuf, MAXPDSTRING);
    STUFF->st_staticpath = namelist_append(0, pathbuf, 0);
    sys_expandpath("%CommonProgramFiles%/Pd", pathbuf, MAXPDSTRING);
    STUFF->st_staticpath = namelist_append(STUFF->st_staticpath, pathbuf, 0);
#endif
    /* add built-in "extra" path last so its checked last */
    STUFF->st_staticpath = namelist_append(STUFF->st_staticpath, p, 0);
}

    /* try to open a file in the directory "dir", named "name""ext",
    for reading.  "Name" may have slashes.  The directory is copied to
    "dirresult" which must be at least "size" bytes.  "nameresult" is set
    to point to the filename (copied elsewhere into the same buffer).
    The "bin" flag requests opening for binary (which only makes a difference
    on Windows). */

int sys_trytoopenone(const char *dir, const char *name, const char* ext,
    char *dirresult, char **nameresult, unsigned int size, int bin)
{
    int fd;
    char buf[MAXPDSTRING];
    if (strlen(dir) + strlen(name) + strlen(ext) + 4 > size)
        return (-1);
    sys_expandpath(dir, buf, MAXPDSTRING);
    strcpy(dirresult, buf);
    if (*dirresult && dirresult[strlen(dirresult)-1] != '/')
        strcat(dirresult, "/");
    strcat(dirresult, name);
    strcat(dirresult, ext);

    DEBUG(post("looking for %s",dirresult));
        /* see if we can open the file for reading */
    if ((fd=sys_open(dirresult, O_RDONLY)) >= 0)
    {
            /* in unix, further check that it's not a directory */
#ifdef HAVE_UNISTD_H
        struct stat statbuf;
        int ok =  ((fstat(fd, &statbuf) >= 0) &&
            !S_ISDIR(statbuf.st_mode));
        if (!ok)
        {
            if (sys_verbose) post("tried %s; stat failed or directory",
                dirresult);
            close (fd);
            fd = -1;
        }
        else
#endif
        {
            char *slash;
            if (sys_verbose) post("tried %s and succeeded", dirresult);
            sys_unbashfilename(dirresult, dirresult);
            slash = strrchr(dirresult, '/');
            if (slash)
            {
                *slash = 0;
                *nameresult = slash + 1;
            }
            else *nameresult = dirresult;

            return (fd);
        }
    }
    else
    {
        if (sys_verbose) post("tried %s and failed", dirresult);
    }
    return (-1);
}

    /* check if we were given an absolute pathname, if so try to open it
    and return 1 to signal the caller to cancel any path searches */
int sys_open_absolute(const char *name, const char* ext,
    char *dirresult, char **nameresult, unsigned int size, int bin, int *fdp)
{
    if (sys_isabsolutepath(name))
    {
        char dirbuf[MAXPDSTRING], *z = strrchr(name, '/');
        int dirlen;
        if (!z)
            return (0);
        dirlen = (int)(z - name);
        if (dirlen > MAXPDSTRING-1)
            dirlen = MAXPDSTRING-1;
        strncpy(dirbuf, name, dirlen);
        dirbuf[dirlen] = 0;
        *fdp = sys_trytoopenone(dirbuf, name+(dirlen+1), ext,
            dirresult, nameresult, size, bin);
        return (1);
    }
    else return (0);
}

/* search for a file in a specified directory, then along the globally
defined search path, using ext as filename extension.  The
fd is returned, the directory ends up in the "dirresult" which must be at
least "size" bytes.  "nameresult" is set to point to the filename, which
ends up in the same buffer as dirresult.  Exception:
if the 'name' starts with a slash or a letter, colon, and slash in MSW,
there is no search and instead we just try to open the file literally.  */

/* see also canvas_open() which, in addition, searches down the
canvas-specific path. */

static int do_open_via_path(const char *dir, const char *name,
    const char *ext, char *dirresult, char **nameresult, unsigned int size,
    int bin, t_namelist *searchpath)
{
    t_namelist *nl;
    int fd = -1;

        /* first check if "name" is absolute (and if so, try to open) */
    if (sys_open_absolute(name, ext, dirresult, nameresult, size, bin, &fd))
        return (fd);

        /* otherwise "name" is relative; try the directory "dir" first. */
    if ((fd = sys_trytoopenone(dir, name, ext,
        dirresult, nameresult, size, bin)) >= 0)
            return (fd);

        /* next go through the temp paths from the commandline */
    for (nl = STUFF->st_temppath; nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin)) >= 0)
                return (fd);
        /* next look in built-in paths like "extra" */
    for (nl = searchpath; nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin)) >= 0)
                return (fd);
        /* next look in built-in paths like "extra" */
    if (sys_usestdpath)
        for (nl = STUFF->st_staticpath; nl; nl = nl->nl_next)
            if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
                dirresult, nameresult, size, bin)) >= 0)
                    return (fd);

    *dirresult = 0;
    *nameresult = dirresult;
    return (-1);
}

    /* open via path, using the global search path. */
int open_via_path(const char *dir, const char *name, const char *ext,
    char *dirresult, char **nameresult, unsigned int size, int bin)
{
    return (do_open_via_path(dir, name, ext, dirresult, nameresult,
        size, bin, STUFF->st_searchpath));
}

    /* open a file with a UTF-8 filename
    This is needed because WIN32 does not support UTF-8 filenames, only UCS2.
    Having this function prevents lots of #ifdefs all over the place.
    */
#ifdef _WIN32
int sys_open(const char *path, int oflag, ...)
{
    int i, fd;
    char pathbuf[MAXPDSTRING];
    wchar_t ucs2path[MAXPDSTRING];
    sys_bashfilename(path, pathbuf);
    u8_utf8toucs2(ucs2path, MAXPDSTRING, pathbuf, MAXPDSTRING-1);
    /* For the create mode, Win32 does not have the same possibilities,
     * so we ignore the argument and just hard-code read/write. */
    if (oflag & O_CREAT)
        fd = _wopen(ucs2path, oflag | O_BINARY, _S_IREAD | _S_IWRITE);
    else
        fd = _wopen(ucs2path, oflag | O_BINARY);
    return fd;
}

FILE *sys_fopen(const char *filename, const char *mode)
{
    char namebuf[MAXPDSTRING];
    wchar_t ucs2buf[MAXPDSTRING];
    wchar_t ucs2mode[MAXPDSTRING];
    sys_bashfilename(filename, namebuf);
    u8_utf8toucs2(ucs2buf, MAXPDSTRING, namebuf, MAXPDSTRING-1);
    /* mode only uses ASCII, so no need for a full conversion, just copy it */
    mbstowcs(ucs2mode, mode, MAXPDSTRING);
    return (_wfopen(ucs2buf, ucs2mode));
}
#else
#include <stdarg.h>
int sys_open(const char *path, int oflag, ...)
{
    int i, fd;
    char pathbuf[MAXPDSTRING];
    sys_bashfilename(path, pathbuf);
    if (oflag & O_CREAT)
    {
        mode_t mode;
        int imode;
        va_list ap;
        va_start(ap, oflag);

        /* Mac compiler complains if we just set mode = va_arg ... so, even
        though we all know it's just an int, we explicitly va_arg to an int
        and then convert.
           -> http://www.mail-archive.com/bug-gnulib@gnu.org/msg14212.html
           -> http://bugs.debian.org/647345
        */

        imode = va_arg (ap, int);
        mode = (mode_t)imode;
        va_end(ap);
        fd = open(pathbuf, oflag, mode);
    }
    else
        fd = open(pathbuf, oflag);
    return fd;
}

FILE *sys_fopen(const char *filename, const char *mode)
{
  char namebuf[MAXPDSTRING];
  sys_bashfilename(filename, namebuf);
  return fopen(namebuf, mode);
}
#endif /* _WIN32 */

   /* close a previously opened file
   this is needed on platforms where you cannot open/close resources
   across dll-boundaries, but we provide it for other platforms as well */
int sys_close(int fd)
{
#ifdef _WIN32
    return _close(fd);  /* Bill Gates is a big fat hen */
#else
    return close(fd);
#endif
}

int sys_fclose(FILE *stream)
{
    return fclose(stream);
}

static int do_open_via_helppath(const char *dir, const char *name,
    const char *ext, char *dirresult, char **nameresult, unsigned int size)
{
    int bin=0;
    int fd=-1;
    t_namelist*nl;

        /* first check if "name" is absolute (and if so, try to open) */
    if (sys_open_absolute(name, ext, dirresult, nameresult, size, bin, &fd))
        return (fd);
        /* otherwise "name" is relative; try the directory "dir" first. */
    if ((fd = sys_trytoopenone(dir, name, ext,
        dirresult, nameresult, size, bin)) >= 0)
            return (fd);

        /* next look in temp help paths from the commandline */
    for (nl = namedlist_getlist("helppath.temp"); nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin)) >= 0)
                return (fd);

        /* next look in temp search paths from the commandline */
    for (nl = STUFF->st_temppath; nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin)) >= 0)
                return (fd);

        /* next look in preference help paths */
    for (nl = STUFF->st_helppath; nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin)) >= 0)
                return (fd);

        /* next look in preference search paths */
    for (nl = STUFF->st_searchpath; nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin)) >= 0)
                return (fd);

        /* next look in built-in help paths like "doc/5.reference" */
    if (sys_usestdpath)
        for (nl = namedlist_getlist("helppath.static"); nl; nl = nl->nl_next)
            if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
                dirresult, nameresult, size, bin)) >= 0)
                    return (fd);

        /* next look in built-in search paths like "extra" */
    if (sys_usestdpath)
        for (nl = STUFF->st_staticpath; nl; nl = nl->nl_next)
            if ((fd = sys_trytoopenone(nl->nl_string, name, ext,
                dirresult, nameresult, size, bin)) >= 0)
                    return (fd);

        /* give up */
    *dirresult = 0;
    *nameresult = dirresult;
    return (-1);
}

    /* Open a help file using the help search path.  We expect the ".pd"
    suffix here, even though we have to tear it back off for one of the
    search attempts. */
void open_via_helppath(const char *name, const char *dir)
{
    char localext[MAXPDLOCALESTRING+10]; /* lang + -help..pd */
    char realname[MAXPDSTRING], dirbuf[MAXPDSTRING], *basename;
        /* make up a silly "dir" if none is supplied */
    const char *usedir = (*dir ? dir : "./");
    int fd;


        /* 1. "objectname-help.pd" */
    strncpy(realname, name, MAXPDSTRING-10);
    realname[MAXPDSTRING-10] = 0;
    if (strlen(realname) > 3 && !strcmp(realname+strlen(realname)-3, ".pd"))
        realname[strlen(realname)-3] = 0;

    if(PATHSTUFF->ps_lang_region[0]) {
        snprintf(localext, sizeof(localext), "-help.%s.pd", PATHSTUFF->ps_lang_region);
        if ((fd = do_open_via_helppath(usedir, realname, localext,
                                   dirbuf, &basename, MAXPDSTRING)) >= 0)
            goto gotone;
    }
    if(PATHSTUFF->ps_lang[0]) {
        snprintf(localext, sizeof(localext), "-help.%s.pd", PATHSTUFF->ps_lang);
        if ((fd = do_open_via_helppath(usedir, realname, localext,
                                   dirbuf, &basename, MAXPDSTRING)) >= 0)
            goto gotone;
    }

    if ((fd = do_open_via_helppath(usedir, realname, "-help.pd",
                                   dirbuf, &basename, MAXPDSTRING)) >= 0)
            goto gotone;

        /* 2. "help-objectname.pd" */
    strcpy(realname, "help-");
    strncat(realname, name, MAXPDSTRING-10);
    realname[MAXPDSTRING-1] = 0;
    if ((fd = do_open_via_helppath(usedir, realname, "",
                                   dirbuf, &basename, MAXPDSTRING)) >= 0)
            goto gotone;

    post("sorry, couldn't find help patch for \"%s\"", name);
    return;
gotone:
    close (fd);
    glob_evalfile(0, gensym((char*)basename), gensym(dirbuf));
}

int sys_argparse(int argc, const char **argv);
static int string2args(const char * cmd, int * retArgc, const char *** retArgv);

void sys_doflags(void)
{
    int rcargc=0;
    const char**rcargv = NULL;
    int len;
    int rcode = 0;
    if (!sys_flags)
        sys_flags = &s_;
    len = (int)strlen(sys_flags->s_name);
    if (len > MAXPDSTRING)
    {
        error("flags: %s: too long", sys_flags->s_name);
        return;
    }
    rcode = string2args(sys_flags->s_name, &rcargc, &rcargv);
    if(rcode < 0) {
        error("error#%d while parsing flags", rcode);
        return;
    }

    if (sys_argparse(rcargc, rcargv))
        error("error parsing startup arguments");

    for(len=0; len<rcargc; len++)
        free((void*)rcargv[len]);
    free(rcargv);
}

/* undo pdtl_encodedialog.  This allows dialogs to send spaces, commas,
    dollars, and semis down here. */
t_symbol *sys_decodedialog(t_symbol *s)
{
    char buf[MAXPDSTRING];
    const char *sp = s->s_name;
    int i;
    if (*sp != '+')
        return s;
    else sp++;
    for (i = 0; i < MAXPDSTRING-1; i++, sp++)
    {
        if (!sp[0])
            break;
        if (sp[0] == '+')
        {
            if (sp[1] == '_')
                buf[i] = ' ', sp++;
            else if (sp[1] == '+')
                buf[i] = '+', sp++;
            else if (sp[1] == 'c')
                buf[i] = ',', sp++;
            else if (sp[1] == 's')
                buf[i] = ';', sp++;
            else if (sp[1] == 'd')
                buf[i] = '$', sp++;
            else buf[i] = sp[0];
        }
        else buf[i] = sp[0];
    }
    buf[i] = 0;
    return (gensym(buf));
}

    /* send the user-specified search path to pd-gui */
static void do_gui_setnamelist(const char*listname, t_namelist*nl)
{
    int i;
    sys_gui("set ::tmp_path {}\n");
    for (i = 0; nl; nl = nl->nl_next, i++)
        sys_vgui("lappend ::tmp_path {%s}\n", nl->nl_string);
    sys_vgui("set %s %s\n", listname, "$::tmp_path");
}
void sys_set_searchpaths(void)
{
        /* set the search paths from the prefs */
    do_gui_setnamelist("::sys_searchpath", STUFF->st_searchpath);
        /* send the temp paths from the commandline to pd-gui */
    do_gui_setnamelist("::sys_temppath", STUFF->st_temppath);
        /* send the hard-coded search path to pd-gui */
    do_gui_setnamelist("::sys_staticpath", STUFF->st_staticpath);
}

    /* start a search path dialog window */
void glob_start_path_dialog(t_pd *dummy)
{
    char buf[MAXPDSTRING];

    do_gui_setnamelist("::sys_searchpath", STUFF->st_searchpath);
    snprintf(buf, MAXPDSTRING-1, "pdtk_path_dialog %%s %d %d\n", sys_usestdpath, sys_verbose);
    gfxstub_new(&glob_pdobject, (void *)glob_start_path_dialog, buf);
}


static void do_set_path(const char*listname, int argc, t_atom *argv) {
    namedlist_free(listname);
    while(argc--) {
        t_symbol *s = sys_decodedialog(atom_getsymbolarg(0, 1, argv++));
        if (*s->s_name)
            namedlist_append_files(listname, s->s_name);
    }
}

void glob_set_pathlist(t_pd *dummy, t_symbol *s, int argc, t_atom *argv) {
    s = atom_getsymbolarg(0, argc, argv);
    if(argc<1) {
        bug("set-pathlist");
        return;
    }
    do_set_path(s->s_name, argc-1, argv+1);
}

    /* new values from dialog window */
void glob_path_dialog(t_pd *dummy, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    sys_usestdpath = atom_getfloatarg(0, argc, argv);
    sys_verbose = atom_getfloatarg(1, argc, argv);
    do_set_path("searchpath.main", argc, argv);
}

    /* add one item to search path (intended for use by Deken plugin).
    if "saveit" is > 0, this also saves all settings,
    if "saveit" is < 0, the path is only added temporarily */
void glob_addtopath(t_pd *dummy, t_symbol *path, t_float saveit)
{
    int saveflag = (int)saveit;
    t_symbol *s = sys_decodedialog(path);
    if (*s->s_name)
    {
        if (saveflag < 0)
            STUFF->st_temppath =
                namelist_append_files(STUFF->st_temppath, s->s_name);
        else
            STUFF->st_searchpath =
                namelist_append_files(STUFF->st_searchpath, s->s_name);
        if (saveit > 0)
            sys_savepreferences(0);
    }
}
void glob_addtohelppath(t_pd *dummy, t_symbol *path, t_float saveit)
{
    int saveflag = (int)saveit;
    t_symbol *s = sys_decodedialog(path);
    if (*s->s_name)
    {
        if (saveflag < 0)
            namedlist_append_files("helppath.temp", s->s_name);
        else
            namedlist_append_files("helppath.main", s->s_name);
        if (saveit > 0)
            sys_savepreferences(0);
    }
}

    /* set the global list vars for startup libraries and flags */
void sys_set_startup(void)
{
    int i;
    t_namelist *nl;
    char obuf[MAXPDSTRING];

    sys_vgui("set ::startup_flags [subst -nocommands {%s}]\n",
        (sys_flags? pdgui_strnescape(obuf, MAXPDSTRING, sys_flags->s_name, 0) : ""));
    sys_gui("set ::startup_libraries {}\n");
    for (nl = STUFF->st_externlist, i = 0; nl; nl = nl->nl_next, i++)
        sys_vgui("lappend ::startup_libraries {%s}\n", nl->nl_string);
}

    /* start a startup dialog window */
void glob_start_startup_dialog(t_pd *dummy)
{
    char buf[MAXPDSTRING];
    char obuf[MAXPDSTRING];
    sys_set_startup();
    snprintf(buf, MAXPDSTRING-1, "pdtk_startup_dialog %%s %d {%s}\n", sys_defeatrt,
        (sys_flags? pdgui_strnescape(obuf, MAXPDSTRING, sys_flags->s_name, 0) : ""));
    gfxstub_new(&glob_pdobject, (void *)glob_start_startup_dialog, buf);
}

    /* new values from dialog window */
void glob_startup_dialog(t_pd *dummy, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    namelist_free(STUFF->st_externlist);
    STUFF->st_externlist = 0;
    sys_defeatrt = atom_getfloatarg(0, argc, argv);
    sys_flags = sys_decodedialog(atom_getsymbolarg(1, argc, argv));
    for (i = 0; i < argc-2; i++)
    {
        t_symbol *s = sys_decodedialog(atom_getsymbolarg(i+2, argc, argv));
        if (*s->s_name)
            STUFF->st_externlist =
                namelist_append_files(STUFF->st_externlist, s->s_name);
    }
}


/*
 * the following string2args function is based on from sash-3.8 (the StandAlone SHell)
 * Copyright (c) 2014 by David I. Bell
 * Permission is granted to use, distribute, or modify this source,
 * provided that this copyright notice remains intact.
 */
#define	isBlank(ch)	(((ch) == ' ') || ((ch) == '\t'))
int string2args(const char * cmd, int * retArgc, const char *** retArgv)
{
    int errCode = 1;
    int len = strlen(cmd), argCount = 0;
    char strings[MAXPDSTRING], *cp;
    const char **argTable = 0, **newArgTable;

    if(retArgc) *retArgc = 0;
    if(retArgv) *retArgv = NULL;

        /*
         * Copy the command string into a buffer that we can modify,
         * reallocating it if necessary.
         */
    if(len >= MAXPDSTRING) {
        errCode = 1; goto ouch;
    }
    memset(strings, 0, MAXPDSTRING);
    memcpy(strings, cmd, len);
    cp = strings;

        /* Keep parsing the command string as long as there are any arguments left. */
    while (*cp) {
        const char *cpIn = cp;
        char *cpOut = cp, *argument;
        int quote = '\0';

            /*
             * Loop over the string collecting the next argument while
             * looking for quoted strings or quoted characters.
             */
        while (*cp) {
            int ch = *cp++;

                /* If we are not in a quote and we see a blank then this argument is done. */
            if (isBlank(ch) && (quote == '\0'))
                break;

                /* If we see a backslash then accept the next character no matter what it is. */
            if (ch == '\\') {
                ch = *cp++;
                if (ch == '\0') { /* but only if there is a next char */
                    errCode = 10; goto ouch;
                }
                *cpOut++ = ch;
                continue;
            }

                /* If we were in a quote and we saw the same quote character again then the quote is done. */
            if (ch == quote) {
                quote = '\0';
                continue;
            }

                /* If we weren't in a quote and we see either type of quote character,
                 * then remember that we are now inside of a quote. */
            if ((quote == '\0') && ((ch == '\'') || (ch == '"')))  {
                quote = ch;
                continue;
            }

                /* Store the character. */
            *cpOut++ = ch;
        }

        if (quote) { /* Unmatched quote character */
            errCode = 11; goto ouch;
        }

            /*
             * Null terminate the argument if it had shrunk, and then
             * skip over all blanks to the next argument, nulling them
             * out too.
             */
        if (cp != cpOut)
            *cpOut = '\0';
        while (isBlank(*cp))
            *cp++ = '\0';

        if (!(argument = calloc(1+cpOut-cpIn, 1))) {
            errCode = 22; goto ouch;
        }
        memcpy(argument, cpIn, cpOut-cpIn);

            /* Now reallocate the argument table to hold the argument, add add it. */
        if (!(newArgTable = (const char **) realloc(argTable, (sizeof(const char *) * (argCount + 1))))) {
            free(argument);
            errCode= 23; goto ouch;
        } else argTable = newArgTable;

        argTable[argCount] = argument;

        argCount++;
    }

        /*
         * Null terminate the argument list and return it.
         */
    if (!(newArgTable = (const char **) realloc(argTable, (sizeof(const char *) * (argCount + 1))))) {
        errCode = 23; goto ouch;
    } else argTable = newArgTable;

    argTable[argCount] = NULL;

    if(retArgc) *retArgc = argCount;
    if(retArgv)
        *retArgv = argTable;
    else
        free(argTable);
    return argCount;

 ouch:
    free(argTable);
    return -errCode;
}

void s_path_newpdinstance(void)
{
        /* we claim st_private for now.
         * if others need to put some private data into STUFF as well,
         * we need to add another layer of indirection;
         * but for now we are the only one, so let's keep it simple
         */
    const char *language = getenv("LANG");
    STUFF->st_private = getbytes(sizeof(t_pathstuff));
    memset(PATHSTUFF->ps_lang, 0, sizeof(PATHSTUFF->ps_lang));
    memset(PATHSTUFF->ps_lang_region, 0, sizeof(PATHSTUFF->ps_lang_region));
    if(language) {
        char lang[MAXPDLOCALESTRING];
        int idx, region=0;
        strncpy(lang, language, sizeof(lang));
        lang[sizeof(lang)-1] = 0;
            /* normalize the language string: lowercase; '-' -> '_'; now '.'-suffix */
        for(idx=0; idx<sizeof(lang); idx++) {
            char c = tolower(lang[idx]);
            switch(c) {
            case '-': c='_'; break;
            case '.': c=0; break;
            default: break;
            }
            lang[idx] = c;
            if(!c)
                break;
            if ('_' == c)region=idx;
        }
            /* now i18n for 'C' and 'POSIX' locale */
        if(strcmp("c", lang) && strcmp("posix", lang)) {
            if(region>0) {
                strncpy(PATHSTUFF->ps_lang_region, lang, sizeof(PATHSTUFF->ps_lang_region)-1);
                lang[region] = 0;
            }
            strncpy(PATHSTUFF->ps_lang, lang, sizeof(PATHSTUFF->ps_lang)-1);
        }
    }
#if 0
    dprintf(2, "==========================\n");
    dprintf(2, "LANG       : %s\n", language);
    dprintf(2, "lang       : %s\n", PATHSTUFF->ps_lang);
    dprintf(2, "lang_region: %s\n", PATHSTUFF->ps_lang_region);
    dprintf(2, "==========================\n");
#endif
}

void s_path_freepdinstance(void)
{
    t_namedlist *namedlist = PATHSTUFF->ps_namedlists;
    while(namedlist) {
        t_namedlist*nl = namedlist;
        namedlist = namedlist->next;
        namelist_free(nl->list);
        freebytes(nl, sizeof(*nl));
    }

    freebytes(PATHSTUFF, sizeof(t_pathstuff));
}
