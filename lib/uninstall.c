#include "config.h"
#include "miscfn.h"

#if HAVE_ALLOCA_H
# include <alloca.h>
#endif 

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "install.h"
#include "intl.h"
#include "messages.h"
#include "md5.h"
#include "misc.h"
#include "rpmdb.h"
#include "rpmlib.h"

static char * SCRIPT_PATH = "PATH=/sbin:/bin:/usr/sbin:/usr/bin:"
			                 "/usr/X11R6/bin\nexport PATH\n";

enum fileActions { REMOVE, BACKUP, KEEP };

static int sharedFileCmp(const void * one, const void * two);
static int handleSharedFiles(rpmdb db, int offset, char ** fileList, 
			     char ** fileMd5List, int fileCount, 
			     enum fileActions * fileActions);
static int removeFile(char * file, char state, unsigned int flags, char * md5, 
		      short mode, enum fileActions action, char * rmmess, 
		      int brokenMd5, int test);

static int sharedFileCmp(const void * one, const void * two) {
    if (((struct sharedFile *) one)->secRecOffset <
	((struct sharedFile *) two)->secRecOffset)
	return -1;
    else if (((struct sharedFile *) one)->secRecOffset ==
	((struct sharedFile *) two)->secRecOffset)
	return 0;
    else 
	return 1;
}

int findSharedFiles(rpmdb db, int offset, char ** fileList, int fileCount,
		    struct sharedFile ** listPtr, int * listCountPtr) {
    int i, j;
    struct sharedFile * list = NULL;
    int itemsUsed = 0;
    int itemsAllocated = 0;
    dbiIndexSet matches;

    itemsAllocated = 5;
    list = malloc(sizeof(struct sharedFile) * itemsAllocated);

    for (i = 0; i < fileCount; i++) {
	if (!rpmdbFindByFile(db, fileList[i], &matches)) {
	    for (j = 0; j < matches.count; j++) {
		if (matches.recs[j].recOffset != offset) {
		    if (itemsUsed == itemsAllocated) {
			itemsAllocated += 10;
			list = realloc(list, sizeof(struct sharedFile) * 
					    itemsAllocated);
		    }
		    list[itemsUsed].mainFileNumber = i;
		    list[itemsUsed].secRecOffset = matches.recs[j].recOffset;
		    list[itemsUsed].secFileNumber = matches.recs[j].fileNumber;
		    itemsUsed++;
		}
	    }

	    dbiFreeIndexRecord(matches);
	}
    }

    if (itemsUsed) {
	qsort(list, itemsUsed, sizeof(struct sharedFile), sharedFileCmp);
	*listPtr = list;
	*listCountPtr = itemsUsed;
    } else {
	free(list);
	*listPtr = NULL;
	*listCountPtr = 0;
    }

    return 0;
}

static int handleSharedFiles(rpmdb db, int offset, char ** fileList, 
			     char ** fileMd5List, int fileCount, 
			     enum fileActions * fileActions) {
    Header sech = NULL;
    int secOffset = 0;
    struct sharedFile * sharedList;
    int sharedCount;
    char * name, * version, * release;
    int secFileCount;
    char ** secFileMd5List, ** secFileList;
    char * secFileStatesList;
    int type;
    int i;
    int rc = 0;

    if (findSharedFiles(db, offset, fileList, fileCount, &sharedList, 
			&sharedCount)) {
	return 1;
    }

    if (!sharedCount) {
	return 0;
    }

    for (i = 0; i < sharedCount; i++) {
	if (secOffset != sharedList[i].secRecOffset) {
	    if (secOffset) {
		headerFree(sech);
		free(secFileMd5List);
		free(secFileList);
	    }

	    secOffset = sharedList[i].secRecOffset;
	    sech = rpmdbGetRecord(db, secOffset);
	    if (!sech) {
		rpmError(RPMERR_DBCORRUPT, 
			 _("cannot read header at %d for uninstall"), offset);
		rc = 1;
		break;
	    }

	    headerGetEntry(sech, RPMTAG_NAME, &type, (void **) &name, 
		     &secFileCount);
	    headerGetEntry(sech, RPMTAG_VERSION, &type, (void **) &version, 
		     &secFileCount);
	    headerGetEntry(sech, RPMTAG_RELEASE, &type, (void **) &release, 
		     &secFileCount);

	    rpmMessage(RPMMESS_DEBUG, 
			_("package %s-%s-%s contain shared files\n"), 
		    	name, version, release);

	    if (!headerGetEntry(sech, RPMTAG_FILENAMES, &type, 
			  (void **) &secFileList, &secFileCount)) {
		rpmError(RPMERR_DBCORRUPT, "package %s contains no files",
		      name);
		headerFree(sech);
		rc = 1;
		break;
	    }

	    headerGetEntry(sech, RPMTAG_FILESTATES, &type, 
		     (void **) &secFileStatesList, &secFileCount);
	    headerGetEntry(sech, RPMTAG_FILEMD5S, &type, 
		     (void **) &secFileMd5List, &secFileCount);
	}

	rpmMessage(RPMMESS_DEBUG, "file %s is shared\n",
		fileList[sharedList[i].mainFileNumber]);
	
	switch (secFileStatesList[sharedList[i].secFileNumber]) {
	  case RPMFILE_STATE_REPLACED:
	    rpmMessage(RPMMESS_DEBUG, "     file has already been replaced\n");
	    break;

	  case RPMFILE_STATE_NOTINSTALLED:
	    rpmMessage(RPMMESS_DEBUG, "     file was never installed\n");
	    break;
    
	  case RPMFILE_STATE_NETSHARED:
	    rpmMessage(RPMMESS_DEBUG, "     file is netshared (so don't touch it)\n");
	    fileActions[sharedList[i].mainFileNumber] = KEEP;
	    break;
    
	  case RPMFILE_STATE_NORMAL:
	    if (!strcmp(fileMd5List[sharedList[i].mainFileNumber],
			secFileMd5List[sharedList[i].secFileNumber])) {
		rpmMessage(RPMMESS_DEBUG, "    file is truely shared - saving\n");
	    }
	    fileActions[sharedList[i].mainFileNumber] = KEEP;
	    break;
	}
    }

    if (secOffset) {
	headerFree(sech);
	free(secFileMd5List);
	free(secFileList);
    }
    free(sharedList);

    return rc;
}

int rpmRemovePackage(char * prefix, rpmdb db, unsigned int offset, int flags) {
    Header h;
    int i;
    int fileCount;
    char * rmmess, * name, * version, * release;
    char * fnbuffer = NULL;
    dbiIndexSet matches;
    int fnbuffersize = 0;
    int prefixLength = strlen(prefix);
    char ** fileList, ** fileMd5List;
    int type, count;
    uint_32 * fileFlagsList;
    int_16 * fileModesList;
    char * fileStatesList;
    enum { REMOVE, BACKUP, KEEP } * fileActions;
    int scriptArg;

    h = rpmdbGetRecord(db, offset);
    if (!h) {
	rpmError(RPMERR_DBCORRUPT, "cannot read header at %d for uninstall",
	      offset);
	return 1;
    }

    headerGetEntry(h, RPMTAG_NAME, &type, (void **) &name,  &count);
    headerGetEntry(h, RPMTAG_VERSION, &type, (void **) &version,  &count);
    headerGetEntry(h, RPMTAG_RELEASE, &type, (void **) &release,  &count);
    /* when we run scripts, we pass an argument which is the number of 
       versions of this package that will be installed when we are finished */
    if (rpmdbFindPackage(db, name, &matches)) {
	rpmError(RPMERR_DBCORRUPT, "cannot read packages named %s for uninstall",
	      name);
	return 1;
    }
 
    scriptArg = matches.count - 1;
    dbiFreeIndexRecord(matches);

    if (flags & RPMUNINSTALL_TEST) {
	rmmess = "would remove";
    } else {
	rmmess = "removing";
    }

    rpmMessage(RPMMESS_DEBUG, "running preuninstall script (if any)\n");

    if (runScript(prefix, h, RPMTAG_PREUN, scriptArg, 
		 flags & RPMUNINSTALL_NOSCRIPTS)) {
	headerFree(h);
	return 1;
    }
    
    rpmMessage(RPMMESS_DEBUG, "%s files test = %d\n", rmmess, flags & RPMUNINSTALL_TEST);
    if (headerGetEntry(h, RPMTAG_FILENAMES, &type, (void **) &fileList, 
	 &fileCount)) {
	if (prefix[0]) {
	    fnbuffersize = 1024;
	    fnbuffer = alloca(fnbuffersize);
	}

	headerGetEntry(h, RPMTAG_FILESTATES, &type, (void **) &fileStatesList, 
		 &fileCount);
	headerGetEntry(h, RPMTAG_FILEMD5S, &type, (void **) &fileMd5List, 
		 &fileCount);
	headerGetEntry(h, RPMTAG_FILEFLAGS, &type, (void **) &fileFlagsList, 
		 &fileCount);
	headerGetEntry(h, RPMTAG_FILEMODES, &type, (void **) &fileModesList, 
		 &fileCount);

	fileActions = alloca(sizeof(*fileActions) * fileCount);
	for (i = 0; i < fileCount; i++) 
	    if (fileStatesList[i] == RPMFILE_STATE_NOTINSTALLED ||
		fileStatesList[i] == RPMFILE_STATE_NETSHARED) 
		fileActions[i] = KEEP;
	    else
		fileActions[i] = REMOVE;

	handleSharedFiles(db, offset, fileList, fileMd5List, fileCount, fileActions);

	/* go through the filelist backwards to help insure that rmdir()
	   will work */
	for (i = fileCount - 1; i >= 0; i--) {
	    if (strcmp(prefix, "/")) {
		if ((strlen(fileList[i]) + prefixLength + 1) > fnbuffersize) {
		    fnbuffersize = (strlen(fileList[i]) + prefixLength) * 2;
		    fnbuffer = alloca(fnbuffersize);
		}
		strcpy(fnbuffer, prefix);
		strcat(fnbuffer, "/");
		strcat(fnbuffer, fileList[i]);
	    } else {
		fnbuffer = fileList[i];
	    }

	    removeFile(fnbuffer, fileStatesList[i], fileFlagsList[i],
		       fileMd5List[i], fileModesList[i], fileActions[i], 
		       rmmess, !headerIsEntry(h, RPMTAG_RPMVERSION),
		       flags & RPMUNINSTALL_TEST);
	}

	free(fileList);
	free(fileMd5List);
    }

    rpmMessage(RPMMESS_DEBUG, "running postuninstall script (if any)\n");
    runScript(prefix, h, RPMTAG_POSTUN, scriptArg, flags & RPMUNINSTALL_NOSCRIPTS);

    headerFree(h);

    rpmMessage(RPMMESS_DEBUG, "%s database entry\n", rmmess);
    if (!(flags & RPMUNINSTALL_TEST))
	rpmdbRemove(db, offset, 0);

    return 0;
}

int runScript(char * prefix, Header h, int tag, int arg, int norunScripts) {
    int count, type;
    char * script;
    char * fn;
    int fd;
    int isdebug = rpmIsDebug();
    int child;
    int status;
    char upgradeArg[20];
    char * installPrefix = NULL;
    char * installPrefixEnv = NULL;

    sprintf(upgradeArg, "%d", arg);

    if (norunScripts) return 0;

    if (headerGetEntry(h, tag, &type, (void **) &script, &count)) {
	if (headerGetEntry(h, RPMTAG_INSTALLPREFIX, &type, (void **) &installPrefix,
	    	     &count)) {
	    installPrefixEnv = alloca(strlen(installPrefix) + 30);
	    strcpy(installPrefixEnv, "RPM_INSTALL_PREFIX=");
	    strcat(installPrefixEnv, installPrefix);
	}

	fn = tmpnam(NULL);
	rpmMessage(RPMMESS_DEBUG, "script found - running from file %s\n", fn);
	fd = open(fn, O_CREAT | O_RDWR);
	if (!isdebug) unlink(fn);
	if (fd < 0) {
	    rpmError(RPMERR_SCRIPT, 
			_("error creating file for (un)install script"));
	    return 1;
	}
	write(fd, SCRIPT_PATH, strlen(SCRIPT_PATH));
	write(fd, script, strlen(script));
	
	/* run the script via /bin/sh - just feed the commands to the
	   shell as stdin */
	if (!(child = fork())) {
	    if (installPrefixEnv) {
		doputenv(installPrefixEnv);
	    }

	    lseek(fd, 0, SEEK_SET);
	    close(0);
	    dup2(fd, 0);
	    close(fd);

	    if (strcmp(prefix, "/")) {
		rpmMessage(RPMMESS_DEBUG, "performing chroot(%s)\n", prefix);
		chroot(prefix);
		chdir("/");
	    }

	    if (isdebug)
		execl("/bin/sh", "/bin/sh", "-xs", upgradeArg, NULL);
	    else
		execl("/bin/sh", "/bin/sh", "-s", upgradeArg, NULL);
	    _exit(-1);
	}
	close(fd);
	waitpid(child, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
	    rpmError(RPMERR_SCRIPT, _("execution of script failed"));
	    return 1;
	}
    }

    return 0;
}

static int removeFile(char * file, char state, unsigned int flags, char * md5, 
		      short mode, enum fileActions action, char * rmmess, 
		      int brokenMd5, int test) {
    char currentMd5[40];
    int rc = 0;
    char * newfile;
	
    switch (state) {
      case RPMFILE_STATE_REPLACED:
	rpmMessage(RPMMESS_DEBUG, "%s has already been replaced\n", file);
	break;

      case RPMFILE_STATE_NORMAL:
	if ((action == REMOVE) && (flags & RPMFILE_CONFIG)) {
	    /* if it's a config file, we may not want to remove it */
	    rpmMessage(RPMMESS_DEBUG, "finding md5sum of %s\n", file);
	    if (brokenMd5)
		rc = mdfileBroken(file, currentMd5);
	    else
		rc = mdfile(file, currentMd5);

	    if (mdfile(file, currentMd5)) 
		rpmMessage(RPMMESS_DEBUG, 
				"    failed - assuming file removed\n");
	    else {
		if (strcmp(currentMd5, md5)) {
		    rpmMessage(RPMMESS_DEBUG, "    file changed - will save\n");
		    action = BACKUP;
		} else {
		    rpmMessage(RPMMESS_DEBUG, 
				"    file unchanged - will remove\n");
		}
	    }
	}

	switch (action) {

	  case KEEP:
	    rpmMessage(RPMMESS_DEBUG, "keeping %s\n", file);
	    break;

	  case BACKUP:
	    rpmMessage(RPMMESS_DEBUG, "saving %s as %s.rpmsave\n", file, file);
	    if (!test) {
		newfile = alloca(strlen(file) + 20);
		strcpy(newfile, file);
		strcat(newfile, ".rpmsave");
		if (rename(file, newfile)) {
		    rpmError(RPMERR_RENAME, _("rename of %s to %s failed: %s"),
				file, newfile, strerror(errno));
		    rc = 1;
		}
	    }
	    break;

	  case REMOVE:
	    rpmMessage(RPMMESS_DEBUG, "%s - %s\n", file, rmmess);
	    if (S_ISDIR(mode)) {
		if (!test) {
		    if (rmdir(file)) {
			if (errno == ENOTEMPTY)
			    rpmError(RPMERR_RMDIR, 
				_("cannot remove %s - directory not empty"), 
				file);
			else
			    rpmError(RPMERR_RMDIR, _("rmdir of %s failed: %s"),
					file, strerror(errno));
			rc = 1;
		    }
		}
	    } else {
		if (!test) {
		    if (unlink(file)) {
			rpmError(RPMERR_UNLINK, _("removal of %s failed: %s"),
				    file, strerror(errno));
			rc = 1;
		    }
		}
	    }
	    break;
	}
   }
 
   return 0;
}
