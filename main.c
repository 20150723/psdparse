/*
    This file is part of "psdparse"
    Copyright (C) 2004-9 Toby Thain, toby@telegraphics.com.au

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifdef powerc // MPW
	#include "getopt.h"
#else
	#include <getopt.h>
#endif

#include "psdparse.h"

#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#if defined(HAVE_SYS_MMAN_H) || defined(WIN32)
	#define CAN_MMAP
#endif

extern int nwarns;
extern char indir[];

char *pngdir = indir;
int verbose = DEFAULT_VERBOSE, quiet = 0, rsrc = 0, extra = 0,
	scavenge = 0, scavenge_psb = 0, scavenge_depth = 8, scavenge_mode = -1,
	scavenge_rows = 0, scavenge_cols = 0, scavenge_chan = 3, scavenge_rle = 0,
	makedirs = 0, numbered = 0, help = 0, split = 0, xmlout = 0, bom = 0;
long hres, vres; // we don't use these, but they're set within doresources()

#ifdef ALWAYS_WRITE_PNG
	// for the Windows console app, we want to be able to drag and drop a PSD
	// giving us no way to specify a destination directory, so use a default
	int writepng = 1, writelist = 1, writexml = 1;
#else
	int writepng = 0, writelist = 0, writexml = 0;
#endif

void usage(char *prog, int status){
	fprintf(stderr, "usage: %s [options] psdfile...\n\
  -h, --help         show this help\n\
  -v, --verbose      print more information\n\
  -q, --quiet        work silently\n\
  -r, --resources    process 'image resources' metadata\n\
  -e, --extra        process 'additional data' (non-image layers, v4 and later)\n\
  -w, --writepng     write PNG files of each raster layer (and merged composite)\n\
  -n, --numbered     use 'layerNN' name for file, instead of actual layer name\n\
  -d, --pngdir dir   put PNGs in specified directory (implies --writepng)\n\
  -m, --makedirs     create subdirectory for PNG if layer name contains %c's\n\
  -l, --list         write an 'asset list' of layer sizes and positions\n\
  -x, --xml          write XML describing document, layers, and any output files\n\
      --xmlout       direct XML to standard output (implies --xml and --quiet)\n\
  -s, --split        write each composite channel to individual (grey scale) PNG\n"
#ifdef CAN_MMAP
"      --scavenge     ignore file header, search entire file for image layers\n\
         --psb           for scavenge, assume PSB (default PSD)\n\
         --depth N       for scavenge, assume this bit depth (default %d)\n\
         --mode N        for scavenge, assume this mode (optional)\n\
         --mergedrows N  to scavenge merged image, row count must be known\n\
         --mergedcols N  to scavenge merged image, column count must be known\n\
         --mergedchan N  to scavenge merged image, channel count must be known (default %d)\n\
      --scavengerle  look for channel RLE counts\n"
#endif
	        , prog, DIRSEP, scavenge_depth, scavenge_chan);
	exit(status);
}

int main(int argc, char *argv[]){
	static struct option longopts[] = {
		{"help",       no_argument, &help, 1},
		{"verbose",    no_argument, &verbose, 1},
		{"quiet",      no_argument, &quiet, 1},
		{"resources",  no_argument, &rsrc, 1},
		{"extra",      no_argument, &extra, 1},
		{"writepng",   no_argument, &writepng, 1},
		{"numbered",   no_argument, &numbered, 1},
		{"pngdir",     required_argument, NULL, 'd'},
		{"makedirs",   no_argument, &makedirs, 1},
		{"list",       no_argument, &writelist, 1},
		{"xml",        no_argument, &writexml, 1},
		{"xmlout",     no_argument, &xmlout, 1},
		{"split",      no_argument, &split, 1},
#ifdef CAN_MMAP
		{"scavenge",   no_argument, &scavenge, 1},
		{"scavengerle",no_argument, &scavenge_rle, 1},
		{"psb",        no_argument, &scavenge_psb, 1},
		{"depth",      required_argument, NULL, 'D'},
		{"mode",       required_argument, NULL, 'M'},
		{"mergedrows", required_argument, NULL, 'R'},
		{"mergedcols", required_argument, NULL, 'C'},
		{"mergedchan", required_argument, NULL, 'H'},
#endif
		{NULL,0,NULL,0}
	};
	FILE *f;
	int i, j, indexptr, opt, fd, map_flag;
	struct psd_header h;
	psd_bytes_t k;
	char *base;
	void *addr = NULL;
	struct stat sb;
	char temp_str[PATH_MAX];

	while( (opt = getopt_long(argc, argv, "hvqrewnd:mlxs", longopts, &indexptr)) != -1 )
		switch(opt){
		case 0: break; // long option
		case 'h': help = 1; break;
		case 'v': verbose = 1; break;
		case 'q': quiet = 1; break;
		case 'r': rsrc = 1; break;
		case 'e': extra = 1; break;
		case 'd': pngdir = optarg; // fall through
		case 'w': writepng = 1; break;
		case 'n': numbered = 1; break;
		case 'm': makedirs = 1; break;
		case 'l': writelist = 1; break;
		case 'x': writexml = 1; break;
		case 's': split = 1; break;
		case 'D': scavenge_depth = atoi(optarg); break;
		case 'M': scavenge_mode  = atoi(optarg); break;
		case 'R': scavenge_rows  = atoi(optarg); break;
		case 'C': scavenge_cols  = atoi(optarg); break;
		case 'H': scavenge_chan  = atoi(optarg); break;
		default:  usage(argv[0], EXIT_FAILURE);
		}

	if(optind >= argc)
		usage(argv[0], EXIT_FAILURE);
	else if(help)
		usage(argv[0], EXIT_SUCCESS);

	for(i = optind; i < argc; ++i){
		if( (f = fopen(argv[i], "rb")) ){
			nwarns = 0;

			if(!quiet && !xmlout)
				printf("Processing \"%s\"\n", argv[i]);

			base = strrchr(argv[i], DIRSEP);

			h.version = h.nlayers = 0;
			h.layerdatapos = 0;

#ifdef CAN_MMAP
			// need to memory map the file, for scavenging routines?
			fd = fileno(f);
			map_flag = (scavenge || scavenge_psb || scavenge_rle)
					   && fstat(fd, &sb) == 0
					   && (sb.st_mode & S_IFMT) == S_IFREG;
			if(map_flag && !(addr = map_file(fd, sb.st_size)))
				fprintf(stderr, "mmap() failed: %d\n", errno);

			if((scavenge || scavenge_psb) && addr)
			{
				h.version = 1 + scavenge_psb;
				h.channels = scavenge_chan;
				h.rows = scavenge_rows;
				h.cols = scavenge_cols;
				h.depth = scavenge_depth;
				h.mode = scavenge_mode;
				scavenge_psd(addr, sb.st_size, &h);

				openfiles(argv[i], &h);

				if(xml){
					fputs("<PSD FILE='", xml);
					fputsxml(argv[i], xml);
					fputs("'>\n", xml);
				}

				for(j = 0; j < h.nlayers; ++j){
					fseeko(f, h.linfo[j].filepos, SEEK_SET);
					readlayerinfo(f, &h, j);
				}

				h.layerdatapos = ftello(f);

				// Layer content starts immediately after the last layer's 'metadata'.
				// If we did not correctly locate the *last* layer, we are not going to
				// succeed in extracting data for any layer.
				processlayers(f, &h);

				// if no layers found, try to locate merged data
				if(!h.nlayers && h.rows && h.cols && h.lmistart){
					// position file after 'layer & mask info'
					fseeko(f, h.lmistart + h.lmilen, SEEK_SET);
					// process merged (composite) image data
					doimage(f, NULL, base ? base+1 : argv[i], h.channels, h.rows, h.cols, &h);
				}
			}
			else
#endif

			if(dopsd(f, argv[i], &h))
			{
				// FIXME: a lot of data structures malloc'd in dopsd()
				// and dolayermaskinfo() are never free'd

				h.layerdatapos = ftello(f);
				UNQUIET("## layer image data begins @ " LL_L("%lld","%ld") "\n", h.layerdatapos);

				// process the layers in 'image data' section,
				// creating PNG/raw files if requested

				processlayers(f, &h);
				skipblock(f, "global layer mask info");

				// global 'additional info' (not really documented)
				// this is found immediately after the 'image data' section
				k = h.lmistart + h.lmilen - ftello(f);
				if(extra)
					doadditional(f, &h, 1, k); // write description to XML

				// position file after 'layer & mask info'
				fseeko(f, h.lmistart + h.lmilen, SEEK_SET);
				// process merged (composite) image data
				doimage(f, NULL, base ? base+1 : argv[i], h.channels, h.rows, h.cols, &h);
			}

#ifdef CAN_MMAP
			if(scavenge_rle && h.nlayers && addr){
				scan_channels(addr, sb.st_size, &h);

				// process scavenged layer channel data
				for(j = 0; j < h.nlayers; ++j)
					if(h.linfo[j].chpos){
						strcpy(temp_str, numbered ? h.linfo[j].nameno : h.linfo[j].name);
						strcat(temp_str, ".scavenged");
						fseeko(f, h.linfo[j].chpos, SEEK_SET);
						doimage(f, &h.linfo[j], temp_str, h.linfo[j].channels,
								h.linfo[j].bottom - h.linfo[j].top,
								h.linfo[j].right - h.linfo[j].left,
								&h);
					}
			}

			if(map_flag)
				unmap_file(addr, sb.st_size); // needed for Windows cleanup, even if mmap() failed
#endif

			if(listfile){
				fputs("}\n", listfile);
				fclose(listfile);
			}
			if(xml){
				fputs("</PSD>\n", xml);
				fclose(xml);
			}
			UNQUIET("  done.\n\n");

#ifdef HAVE_ICONV_H
			extern iconv_t ic;
			if(ic != (iconv_t)-1) iconv_close(ic);
#endif
			fclose(f);
		}else
			alwayswarn("# \"%s\": couldn't open\n", argv[i]);
	}
	return EXIT_SUCCESS;
}
