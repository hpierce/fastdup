#include "util.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <cstdlib>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <map>
#include <string>
#include <vector>
#include <math.h>

struct FileReference
{
	const char *dir;
	char *file;
	FileReference *next;
};

std::map<off_t,FileReference*> SizeMap;
std::vector<FileReference*> SizeDups;

void ScanDirectory(const char *path);
void DeepCompare(FileReference *first);

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("Usage: %s <directory>\n", argv[0]);
		return EXIT_FAILURE;
	}
	
	/* Initial scan - this step will recurse through the directory tree(s)
	 * and find each file we will be working with. These files are mapped
	 * by their size as this process runs through, so we will be provided
	 * with a list of files with the same sizes at the end. Those files
	 * are what we select for the deep comparison, which is where the magic
	 * really shows ;) */
	for (int i = 1; i < argc; i++)
		ScanDirectory(argv[i]);
	
	printf("Initial scanning complete on %d files\n", SizeMap.size());
	printf("Running deep scan on %d sets of files\n\n", SizeDups.size());
	
	SizeMap.clear();
	
	for (std::vector<FileReference*>::iterator it = SizeDups.begin(); it != SizeDups.end(); ++it)
	{
		DeepCompare(*it);
	}
	
	return EXIT_SUCCESS;
}

void ScanDirectory(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
	{
		printf("opendir error (%s): %s\n", path, strerror(errno));
		return;
	}
	
	char *tmp = new char[strlen(path) + 1];
	strcpy(tmp, path);
	path = tmp;
	
	struct dirent *de;
	struct stat st;
	int dfd = dirfd(d);
	while ((de = readdir(d)) != NULL)
	{
		if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2])))
			continue;
		
		if (fstatat(dfd, de->d_name, &st, 0) < 0)
		{
			printf("stat failure (%s/%s): %s\n", path, de->d_name, strerror(errno));
			continue;
		}
		
		if (S_ISREG(st.st_mode))
		{
			FileReference *ref = new FileReference();
			ref->dir = path;
			ref->file = new char[de->d_reclen + 1];
			strcpy(ref->file, de->d_name);
			ref->next = NULL;
			
			std::map<off_t,FileReference*>::iterator it = SizeMap.find(st.st_size);
			if (it == SizeMap.end())
			{
				SizeMap.insert(std::make_pair(st.st_size, ref));
			}
			else if (!it->second->next)
			{
				it->second->next = ref;
				SizeDups.push_back(it->second);
			}
			else
			{
				FileReference *i = it->second;
				while (i->next != NULL)
					i = i->next;
				i->next = ref;
			}
		}
		else if (S_ISDIR(st.st_mode))
		{
			ScanDirectory((std::string(path) + "/" + de->d_name).c_str());
		}
		else
		{
			printf("Skipped (non-file): %s/%s\n", path, de->d_name);
		}
	}
	
	closedir(d);
}

/* Deep comparison is the clever technique upon which the entire
 * concept of fastdup is based.
 *
 * The normal method of comparing files is to hash them and compare
 * their hashes - this has the benefit of being O(n) (whereas
 * straight comparisons are O(n^n)), but is somewhat CPU intensive
 * (though usually still disk bound) and not infallable (collisions).
 * The real problem with hashing when used on large filesets is that,
 * while it can run in O(n) time, it cannot run in anything less.
 * That is, every file must be hashed entirely. When dealing with
 * large files or large numbers of files, this can be come seriously
 * slow.
 *
 * Deep comparison compares files byte by byte, but does so very
 * intelligently. We've already got files in sets by filesize, which
 * dramatically reduces the number of comparisons necessary. The
 * idea is to compare every file to every other file in blocks, but
 * with the use of a number of tricks to reduce the number of
 * comparisons actually necessary. As a result, this method will work
 * fastest on files that are not duplicates (which will be eliminated
 * as soon as they cannot match anything else), which are far more
 * common in most datasets than non-duplicates. See the code for
 * details on how exactly this is optimized down from needing full
 * O(n^n) comparisons on the contents of every file - there are many
 * methods, some of which are quite intricate.
 */
void DeepCompare(FileReference *first)
{
	// Buffer for quick creation of the file's full path
	char fnbuf[PATH_MAX];
	
	int fcount = 0;
	for (FileReference *p = first; p; p = p->next)
		++fcount;
	
	// FDs
	int ffd[fcount];
	// File reference map, used afterwards to map back to the real file
	FileReference *frmap[fcount];
	// Data buffers
	char *rrdbuf = new char[fcount * 65535];
	char *rdbuf[fcount];
	ssize_t rdbp = 0;
	// Matchflag is true for each file pair that may still match
	bool matchflag[(fcount*(fcount-1))/2];
	int mresult[fcount];
	// Omit is true for files that have no possible matches left
	bool omit[fcount];
	int omitted = 0;
	int skipcount[fcount];
	
	memset(matchflag, true, sizeof(matchflag));
	memset(omit, false, sizeof(omit));
	memset(skipcount, 0, sizeof(skipcount));
	
	int i = 0, j = 0;
	for (FileReference *p = first; p; p = p->next, ++i)
	{
		frmap[i] = p;
		
		PathMerge(fnbuf, sizeof(fnbuf), p->dir, p->file);
		
		rdbuf[i] = rrdbuf + (65535 * i);
		
		if ((ffd[i] = open(fnbuf, O_RDONLY)) < 0)
		{
			printf("Unable to open file '%s': %s\n", fnbuf, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
	
	// Loop over blocks of the files, which will be compared
	for (;;)
	{
		for (i = 0; i < fcount; i++)
		{
			if (omit[i])
				continue;
			
			rdbp = read(ffd[i], rdbuf[i], 65535);
			
			if (rdbp < 0)
			{
				printf("%d: Read error: %s\n", i, strerror(errno));
				exit(EXIT_FAILURE);
			}
			else if (!rdbp)
			{
				// All files are assumed to be equal in size
				break;
			}
		}
		
		if (!rdbp)
			break;
		
		for (i = 0; i < fcount; i++)
		{
			if (omit[i])
				continue;
			
			for (j = i + 1; j < fcount; j++)
			{
				if (omit[j])
					continue;
				
				int flagpos = int(((fcount-1)*i)-(i*(i/2.0-0.5))+(j-i)-1);
				if (!matchflag[flagpos])
					continue;
				
				mresult[j] = memcmp(rdbuf[i], rdbuf[j], rdbp);
				
				for (int k = j - 1; k > i; --k)
				{
					if (omit[k])
						continue;
					
					int kflagpos = int(((fcount-1)*k)-(k*(k/2.0-0.5))+(j-k)-1);
					if (!matchflag[kflagpos])
						continue;
					
					if (mresult[k] != mresult[j])
					{
						matchflag[kflagpos] = 0;
						++skipcount[j];
						if (++skipcount[k] == fcount - 1)
						{
							omit[k] = true;
							close(ffd[k]);
							ffd[k] = -1;
							omitted++;
						}
					}
				}
				
				if (mresult[j] != 0)
				{
					matchflag[flagpos] = 0;
					skipcount[i]++;
					if (++skipcount[j] == fcount - 1)
					{
						omit[j] = true;
						close(ffd[j]);
						ffd[j] = -1;
						omitted++;
					}
				}
			}
			
			if (skipcount[i] == fcount - 1)
			{
				omit[i] = true;
				close(ffd[i]);
				ffd[i] = -1;
				if (++omitted == fcount)
					goto endscan;
			}
		}
	}
	
 endscan:
 	delete []rrdbuf;
 	
 	if (omitted == fcount)
 	{
 		// No matches
 		return;
 	}
 	
	for (i = 0; i < fcount; i++)
	{
		if (omit[i])
			continue;
		
		close(ffd[i]);
		
		for (int j = i + 1; j < fcount; j++)
		{
			if (!omit[j] && matchflag[int(((fcount-1)*i)-(i*(i/2.0-0.5))+(j-i)-1)])
			{
				PathMerge(fnbuf, sizeof(fnbuf), frmap[i]->dir, frmap[i]->file);
				printf("\t%s\n", fnbuf);
				PathMerge(fnbuf, sizeof(fnbuf), frmap[j]->dir, frmap[j]->file);
				printf("\t%s\n\n", fnbuf);
			}
		}
	}
}
