/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 *
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * GROningen Mixture of Alchemy and Childrens' Stories
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <stdio.h>

#include "gmx_fatal.h"
#include "macros.h"
#include "smalloc.h"
#include "futil.h"
#include "filenm.h"
#include "string2.h"
#include "gmxfio.h"

#ifdef GMX_THREADS
#include "thread_mpi.h"
#endif


/* The source code in this file should be thread-safe. 
 * But some functions are NOT THREADSAFE when multiple threads
 * use the same file pointer.
 * Please keep it that way.
 */

/* XDR should be available on all platforms now, 
 * but we keep the possibility of turning it off...
 */
#define USE_XDR

typedef struct {
  int  iFTP;
  bool bOpen,bRead,bDouble,bDebug,bStdio;
  char *fn;
  FILE *fp;
  XDR  *xdr;
  bool bLargerThan_off_t;
} t_fileio;


/* These simple lists define the I/O type for these files */
static const int ftpXDR[] = { efTPR, efTRR, efEDR, efXTC, efMTX, efCPT };
static const int ftpASC[] = { efTPA, efGRO, efPDB };
static const int ftpBIN[] = { efTPB, efTRJ };
#ifdef HAVE_XML
static const int ftpXML[] = { efXML };
#endif

bool in_ftpset(int ftp,int nset,const int set[])
{
  int i;
  bool bResult;
  
  bResult = FALSE;
  for(i=0; (i<nset); i++)
    if (ftp == set[i])
      bResult = TRUE;
  
  return bResult;    
}

static bool do_dummy(void *item,int nitem,int eio,
		     const char *desc,const char *srcfile,int line)
{
  gmx_fatal(FARGS,"gmx_fio_select not called!");
  
  return FALSE;
}

/* Global variables */
do_func *do_read  = do_dummy;
do_func *do_write = do_dummy;
const char *itemstr[eitemNR] = {
  "[header]",      "[inputrec]",   "[box]",         "[topology]", 
  "[coordinates]", "[velocities]", "[forces]"
};
/* Comment strings for TPA only */
const char *comment_str[eitemNR] = {
  "; The header holds information on the number of atoms etc. and on whether\n"
  "; certain items are present in the file or not.\n"
  "; \n"
  ";                             WARNING\n"
  ";                   DO NOT EDIT THIS FILE BY HAND\n"
  "; The GROMACS preprocessor performs a lot of checks on your input that\n"
  "; you ignore when editing this. Your simulation may crash because of this\n",
  
  "; The inputrec holds the parameters for MD such as the number of steps,\n"
  "; the timestep and the cut-offs.\n",
  "; The simulation box in nm.\n",
  "; The topology section describes the topology of the molcecules\n"
  "; i.e. bonds, angles and dihedrals etc. and also holds the force field\n"
  "; parameters.\n",
  "; The atomic coordinates in nm\n",
  "; The atomic velocities in nm/ps\n",
  "; The forces on the atoms in nm/ps^2\n"
};


/* Local variables */
static t_fileio *FIO = NULL;
static t_fileio *curfio = NULL;
static int  nFIO = 0;
static const char *eioNames[eioNR] = { "REAL", "INT", "GMX_STE_T",
	     			       "UCHAR", "NUCHAR", "USHORT", 
				       "RVEC", "NRVEC", "IVEC", "STRING" };
static char *add_comment = NULL;

#ifdef GMX_THREADS
static tMPI_Thread_mutex_t fio_mutex=TMPI_THREAD_MUTEX_INITIALIZER;
/* this mutex locks concurrent access to the FIO and curfio arrays, the
   nFIO counter, and the add_comment string.  For now this is 
   the easiest way to make this all thread-safe. Because I/O is mostly
   done by the master node, this won't cause any performance issues 
   (locking/unlocking mutexes is very cheap as long as no thread get 
   scheduled out). */
#endif

/* these functions are all called from functions that lock the fio_mutex
   themselves, and need to make sure that the called function doesn't 
   try to lock that mutex again. */
static int gmx_fio_flush_lock(int fio, bool do_lock);
static int gmx_fio_close_lock(int fio, bool do_lock);
static bool do_xdr_lock(void *item,int nitem,int eio,
                        const char *desc,const char *srcfile,int line, 
                        bool do_lock);



static const char *dbgstr(const char *desc)
{
  static const char *null_str="";
  static char buf[STRLEN];
  
  if (!curfio->bDebug)
    return null_str;
  else {
    sprintf(buf,"  ; %s %s",add_comment ? add_comment : "",desc);
    return buf;
  }
}

void set_comment(const char *comment)
{
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    if (comment)
        add_comment = strdup(comment);
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
}

void unset_comment(void)
{
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    if (add_comment)
        sfree(add_comment);
    add_comment = NULL;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
}


static void _check_nitem(int eio,int nitem,const char *file,int line)
{
  if ((nitem != 1) && !((eio == eioNRVEC) || (eio == eioNUCHAR)))
    gmx_fatal(FARGS,"nitem (%d) may differ from 1 only for %s or %s, not for %s"
	      "(%s, %d)",nitem,eioNames[eioNUCHAR],eioNames[eioNRVEC],
	      eioNames[eio],file,line);
}

#define check_nitem() _check_nitem(eio,nitem,__FILE__,__LINE__)

static void fe(int eio,const char *desc,const char *srcfile,int line)
{

  gmx_fatal(FARGS,"Trying to %s %s type %d (%s), src %s, line %d",
	    curfio->bRead ? "read" : "write",desc,eio,
	    ((eio >= 0) && (eio < eioNR)) ? eioNames[eio] : "unknown",
	    srcfile,line);
}

#define FE() fe(eio,desc,__FILE__,__LINE__)


static void encode_string(int maxlen,char dst[],char src[])
{
  int i;
  
  for(i=0; (src[i] != '\0') && (i < maxlen-1); i++)
    if ((src[i] == ' ') || (src[i] == '\t'))
      dst[i] = '_';
    else
      dst[i] = src[i];
  dst[i] = '\0';
  
  if (i == maxlen)
    fprintf(stderr,"String '%s' truncated to '%s'\n",src,dst);
}

static void decode_string(int maxlen,char dst[],char src[])
{
  int i;
  
  for(i=0; (src[i] != '\0') && (i < maxlen-1); i++)
    if (src[i] == '_')
      dst[i] = ' ';
    else
      dst[i] = src[i];
  dst[i] = '\0';
  
  if (i == maxlen)
    fprintf(stderr,"String '%s' truncated to '%s'\n",src,dst);
}


static bool do_ascwrite(void *item,int nitem,int eio,
			const char *desc,const char *srcfile,int line)
{
  int  i;
  int  res=0,*iptr;
  real *ptr;
  char strbuf[256];
  unsigned char *ucptr;
  
#ifdef GMX_THREADS
  tMPI_Thread_mutex_lock(&fio_mutex);
#endif
  check_nitem();
  switch (eio) {
  case eioREAL:
  case eioDOUBLE:
    res = fprintf(curfio->fp,"%18.10e%s\n",*((real *)item),dbgstr(desc));
    break;
  case eioINT:
    res = fprintf(curfio->fp,"%18d%s\n",*((int *)item),dbgstr(desc));
    break;
  case eioGMX_STEP_T:
    sprintf(strbuf,"%s%s%s","%",gmx_step_fmt,"\n");
    res = fprintf(curfio->fp,strbuf,*((gmx_step_t *)item),dbgstr(desc));
    break;
  case eioUCHAR:
    res = fprintf(curfio->fp,"%4d%s\n",*((unsigned char *)item),dbgstr(desc));
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    for(i=0; (i<nitem); i++)
      res = fprintf(curfio->fp,"%4d",(int)ucptr[i]);
    fprintf(curfio->fp,"%s\n",dbgstr(desc));
    break;
  case eioUSHORT:
    res = fprintf(curfio->fp,"%18d%s\n",*((unsigned short *)item),
		  dbgstr(desc));
    break;
  case eioRVEC:
    ptr = (real *)item;
    res = fprintf(curfio->fp,"%18.10e%18.10e%18.10e%s\n",
		  ptr[XX],ptr[YY],ptr[ZZ],dbgstr(desc));
    break;
  case eioNRVEC:
    for(i=0; (i<nitem); i++) {
      ptr = ((rvec *)item)[i];
      res = fprintf(curfio->fp,"%18.10e%18.10e%18.10e%s\n",
		    ptr[XX],ptr[YY],ptr[ZZ],dbgstr(desc));
    }
    break;
  case eioIVEC:
    iptr= (int *)item;
    res = fprintf(curfio->fp,"%18d%18d%18d%s\n",
		  iptr[XX],iptr[YY],iptr[ZZ],dbgstr(desc));
    break;
  case eioSTRING:
    encode_string(256,strbuf,(char *)item);
    res = fprintf(curfio->fp,"%-18s%s\n",strbuf,dbgstr(desc));
    break;
  default:
    FE();
  }
  if ((res <= 0) && curfio->bDebug)
    fprintf(stderr,"Error writing %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
#ifdef GMX_THREADS
  tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
  return (res > 0);
}

/* This is a global variable that is reset when a file is opened. */
/*static  int  nbuf=0;*/

static char *next_item(FILE *fp, char *buf, int buflen)
{
    int rd;
    bool in_comment=FALSE;
    bool in_token=FALSE;
    int i=0;
    /* This routine reads strings from the file fp, strips comment
     * and buffers. For thread-safety reasons, It reads through getc()  */

    rd=getc(fp);
    if (rd==EOF)
        gmx_file("End of file");
    do
    {
        if (in_comment)
        {
            if (rd=='\n')
                in_comment=FALSE;
        }
        else if (in_token)
        {
            if (isspace(rd) || rd==';')
                break;
            buf[i++]=(char)rd;
        }
        else
        {
            if (!isspace(rd))
            {
                if (rd==';')
                    in_comment=TRUE;
                else 
                {
                    in_token=TRUE;
                    buf[i++]=(char)(rd);
                }
            }
        }
        if (i >= buflen-2)
            break;
    } while( (rd=getc(fp)) != EOF );

    fprintf(stderr,"WARNING, ftpASC file type not tested!\n");

    buf[i]=0;

    return buf;
}

static bool do_ascread(void *item,int nitem,int eio,
		       const char *desc,const char *srcfile,int line)
{
  FILE   *fp = curfio->fp;
  int    i,m,res=0,*iptr,ix;
  gmx_step_t s;
  double d,x;
  real   *ptr;
  unsigned char uc,*ucptr;
  char   *cptr;
#define NEXT_ITEM_BUF_LEN 128
  char   ni_buf[NEXT_ITEM_BUF_LEN];
  
  check_nitem();  
  switch (eio) {
  case eioREAL:
  case eioDOUBLE:
    res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%lf",&d);
    if (item) *((real *)item) = d;
    break;
  case eioINT:
    res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%d",&i);
    if (item) *((int *)item) = i;
    break;
  case eioGMX_STEP_T:
    res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),gmx_step_pfmt,&s);
    if (item) *((gmx_step_t *)item) = s;
    break;
  case eioUCHAR:
    res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%c",&uc);
    if (item) *((unsigned char *)item) = uc;
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    for(i=0; (i<nitem); i++) {
      res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%d",&ix);
      if (item) ucptr[i] = ix;
    }
    break;
  case eioUSHORT:
    res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%d",&i);
    if (item) *((unsigned short *)item) = i;
    break;
  case eioRVEC:
    ptr = (real *)item;
    for(m=0; (m<DIM); m++) {
      res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%lf\n",&x);
      ptr[m] = x;
    }
    break;
  case eioNRVEC:
    for(i=0; (i<nitem); i++) {
      ptr = ((rvec *)item)[i];
      for(m=0; (m<DIM); m++) {
	res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%lf\n",&x);
	if (item) ptr[m] = x;
      }
    }
    break;
  case eioIVEC:
    iptr = (int *)item;
    for(m=0; (m<DIM); m++) {
      res = sscanf(next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN),"%d\n",&ix);
      if (item) iptr[m] = ix;
    }
    break;
  case eioSTRING:
    cptr = next_item(fp,ni_buf,NEXT_ITEM_BUF_LEN);
    if (item) {
      decode_string(strlen(cptr)+1,(char *)item,cptr);
      /* res = sscanf(cptr,"%s",(char *)item);*/
      res = 1;
    }
    break;
  default:
    FE();
  }

#ifdef GMX_THREADS
  tMPI_Thread_mutex_lock(&fio_mutex);
#endif
  if ((res <= 0) && curfio->bDebug)
    fprintf(stderr,"Error reading %s %s from file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
#ifdef GMX_THREADS
  tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
  return (res > 0);
}

static bool do_binwrite(void *item,int nitem,int eio,
			const char *desc,const char *srcfile,int line)
{
  size_t size=0,wsize;
  int    ssize;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    size = sizeof(real);
    break;
  case eioDOUBLE:
    size = sizeof(double);
    break;
  case eioINT:
    size = sizeof(int);
    break;
  case eioGMX_STEP_T:
    size = sizeof(gmx_step_t);
    break;
  case eioUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioNUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioUSHORT:
    size = sizeof(unsigned short);
    break;
  case eioRVEC:
    size = sizeof(rvec);
    break;
  case eioNRVEC:
    size = sizeof(rvec);
    break;
  case eioIVEC:
    size = sizeof(ivec);
    break;
  case eioSTRING:
    size = ssize = strlen((char *)item)+1;
    do_binwrite(&ssize,1,eioINT,desc,srcfile,line);
    break;
  default:
    FE();
  }

  wsize = fwrite(item,size,nitem,curfio->fp);

#ifdef GMX_THREADS
  tMPI_Thread_mutex_lock(&fio_mutex);
#endif
  if ((wsize != nitem) && curfio->bDebug) {
    fprintf(stderr,"Error writing %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
    fprintf(stderr,"written size %u bytes, source size %u bytes\n",
	    (unsigned int)wsize,(unsigned int)size);
  }
#ifdef GMX_THREADS
  tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
  return (wsize == nitem);
}

static bool do_binread(void *item,int nitem,int eio,
		       const char *desc,const char *srcfile,int line)
{
  size_t size=0,rsize;
  int    ssize;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    if (curfio->bDouble)
      size = sizeof(double);
    else
      size = sizeof(float);
    break;
  case eioDOUBLE:
    size = sizeof(double);
    break;
  case eioINT:
    size = sizeof(int);
    break;
  case eioGMX_STEP_T:
    size = sizeof(gmx_step_t);
    break;
  case eioUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioNUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioUSHORT:
    size = sizeof(unsigned short);
    break;
  case eioRVEC:
  case eioNRVEC:
    if (curfio->bDouble)
      size = sizeof(double)*DIM;
    else
      size = sizeof(float)*DIM;
    break;
  case eioIVEC:
    size = sizeof(ivec);
    break;
  case eioSTRING:
    do_binread(&ssize,1,eioINT,desc,srcfile,line);
    size = ssize;
    break;
  default:
    FE();
  }

#ifdef GMX_THREADS
  tMPI_Thread_mutex_lock(&fio_mutex);
#endif
  if (item)
      rsize = fread(item,size,nitem,curfio->fp);
  else {
      /* Skip over it if we have a NULL pointer here */
#ifdef HAVE_FSEEKO
      fseeko(curfio->fp,(off_t)(size*nitem),SEEK_CUR);
#else
      fseek(curfio->fp,(size*nitem),SEEK_CUR);
#endif    
      rsize = nitem;
  }
  if ((rsize != nitem) && (curfio->bDebug))
      fprintf(stderr,"Error reading %s %s from file %s (source %s, line %d)\n",
              eioNames[eio],desc,curfio->fn,srcfile,line);

#ifdef GMX_THREADS
  tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
  return (rsize == nitem);
}

#ifdef USE_XDR

/* this is a recursive function that does mutex locking, so
   there is an a function that locks (do_xdr) and the real function
   that calls itself without locking.  */
static bool do_xdr_lock(void *item,int nitem,int eio,
                        const char *desc,const char *srcfile,int line, 
                        bool do_lock)
{
  unsigned char ucdum,*ucptr;
  bool_t res=0;
  float  fvec[DIM];
  double dvec[DIM];
  int    j,m,*iptr,idum;
  gmx_step_t sdum;
  real   *ptr;
  unsigned short us;
  double d=0;
  float  f=0;
  
#ifdef GMX_THREADS
  if (do_lock)
      tMPI_Thread_mutex_lock(&fio_mutex);
#endif
  check_nitem();
  switch (eio) {
  case eioREAL:
    if (curfio->bDouble) {
      if (item && !curfio->bRead) d = *((real *)item);
      res = xdr_double(curfio->xdr,&d);
      if (item) *((real *)item) = d;
    }
    else {
      if (item && !curfio->bRead) f = *((real *)item);
      res = xdr_float(curfio->xdr,&f);
      if (item) *((real *)item) = f;
    }
    break;
  case eioDOUBLE:
    if (item && !curfio->bRead) d = *((double *)item);
    res = xdr_double(curfio->xdr,&d);
    if (item) *((double *)item) = d;
    break;
  case eioINT:
    if (item && !curfio->bRead) idum = *(int *)item;
    res = xdr_int(curfio->xdr,&idum);
    if (item) *(int *)item = idum;
    break;
  case eioGMX_STEP_T:
    /* do_xdr will not generate a warning when a 64bit gmx_step_t
     * value that is out of 32bit range is read into a 32bit gmx_step_t.
     */
    if (item && !curfio->bRead) sdum = *(gmx_step_t *)item;
    res = xdr_gmx_step_t(curfio->xdr,&sdum,NULL);
    if (item) *(gmx_step_t *)item = sdum;
    break;
  case eioUCHAR:
    if (item && !curfio->bRead) ucdum = *(unsigned char *)item;
		  res = xdr_u_char(curfio->xdr,&ucdum);
    if (item) *(unsigned char *)item = ucdum;
	break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    res   = 1;
    for(j=0; (j<nitem) && res; j++) {
      res = xdr_u_char(curfio->xdr,&(ucptr[j]));
    }
    break;
  case eioUSHORT:
    if (item && !curfio->bRead) us = *(unsigned short *)item;
    res = xdr_u_short(curfio->xdr,(unsigned short *)&us);
    if (item) *(unsigned short *)item = us;
    break;
  case eioRVEC:
    if (curfio->bDouble) {
      if (item && !curfio->bRead)
	for(m=0; (m<DIM); m++) 
	  dvec[m] = ((real *)item)[m];
      res=xdr_vector(curfio->xdr,(char *)dvec,DIM,(unsigned int)sizeof(double),
		     (xdrproc_t)xdr_double);
      if (item)
	for(m=0; (m<DIM); m++) 
	  ((real *)item)[m] = dvec[m];
    }
    else {
      if (item && !curfio->bRead)
	for(m=0; (m<DIM); m++) 
	  fvec[m] = ((real *)item)[m];
      res=xdr_vector(curfio->xdr,(char *)fvec,DIM,(unsigned int)sizeof(float),
		     (xdrproc_t)xdr_float);
      if (item)
	for(m=0; (m<DIM); m++) 
	  ((real *)item)[m] = fvec[m];
    }
    break;
  case eioNRVEC:
    ptr = NULL;
    res = 1;
    for(j=0; (j<nitem) && res; j++) {
      if (item)
	ptr = ((rvec *)item)[j];
      res = do_xdr_lock(ptr,1,eioRVEC,desc,srcfile,line, FALSE);
    }
    break;
  case eioIVEC:
    iptr = (int *)item;
    res  = 1;
    for(m=0; (m<DIM) && res; m++) {
      if (item && !curfio->bRead) idum = iptr[m];
      res = xdr_int(curfio->xdr,&idum);
      if (item) iptr[m] = idum;
    }
    break;
  case eioSTRING: {
    char *cptr;
    int  slen;
    
    if (item) {
      if (!curfio->bRead) 
	slen = strlen((char *)item)+1;
      else
	slen = 0;
    }
    else
      slen = 0;
    
    if (xdr_int(curfio->xdr,&slen) <= 0)
      gmx_fatal(FARGS,"wrong string length %d for string %s"
		" (source %s, line %d)",slen,desc,srcfile,line);
    if (!item && curfio->bRead)
      snew(cptr,slen);
    else
      cptr=(char *)item;
    if (cptr) 
      res = xdr_string(curfio->xdr,&cptr,slen);
    else
      res = 1;
    if (!item && curfio->bRead)
      sfree(cptr);
    break;
  }
  default:
    FE();
  }
  if ((res == 0) && (curfio->bDebug))
    fprintf(stderr,"Error in xdr I/O %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);

#ifdef GMX_THREADS
  if (do_lock)
      tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
  return (res != 0);
}

static bool do_xdr(void *item,int nitem,int eio,
		   const char *desc,const char *srcfile,int line)
{
    /* this is a recursive function that does mutex locking, so
       it needs to be called with locking here, but without locking
       from itself */
    return do_xdr_lock(item, nitem, eio, desc, srcfile, line, TRUE);
}
#endif

#define gmx_fio_check(fio) range_check(fio,0,nFIO)

/*****************************************************************
 *
 *                     EXPORTED SECTION
 *
 *****************************************************************/
int gmx_fio_open(const char *fn,const char *mode)
{
    t_fileio *fio=NULL;
    int      i,nfio=0;
    char     newmode[5];
    bool     bRead;
    int      xdrid;

    if (fn2ftp(fn)==efTPA)
    {
        strcpy(newmode,mode);
    }
    else 
    {
        if (mode[0]=='r')
        {
            strcpy(newmode,"r");
        }
        else if (mode[0]=='w')
        {
            strcpy(newmode,"w");
        }
        else if (mode[0]=='a')
        {
            strcpy(newmode,"a");
        }
        else
        {
            gmx_fatal(FARGS,"DEATH HORROR in gmx_fio_open, mode is '%s'",mode);
        }
    }

    /* Check if it should be opened as a binary file */
    if (strncmp(ftp2ftype(fn2ftp(fn)),"ASCII",5)) 
    {
        /* Not ascii, add b to file mode */
        if ((strchr(newmode,'b')==NULL) && (strchr(newmode,'B')==NULL))
        {
            strcat(newmode,"b");
        }
    }

#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    /* Determine whether we have to make a new one */
    for(i=0; (i<nFIO); i++)
    {
        if (!FIO[i].bOpen) 
        {
            fio  = &(FIO[i]);
            nfio = i;
            break;
        }
    }

    if (i == nFIO) 
    {
        nFIO++;
        srenew(FIO,nFIO);
        fio  = &(FIO[nFIO-1]);
        nfio = nFIO-1;
    }

    bRead = (newmode[0]=='r');
    fio->fp  = NULL;
    fio->xdr = NULL;
    if (fn) 
    {
        fio->iFTP   = fn2ftp(fn);
        fio->fn     = strdup(fn);
        fio->bStdio = FALSE;

        /* If this file type is in the list of XDR files, open it like that */
        if (in_ftpset(fio->iFTP,asize(ftpXDR),ftpXDR)) 
        {
            /* First check whether we have to make a backup,
             * only for writing, not for read or append.
             */
            if (newmode[0]=='w') 
            {
#ifndef GMX_FAHCORE
                /* only make backups for normal gromacs */
                if (gmx_fexist(fn)) 
                {
                    char *bf=(char *)backup_fn(fn);
                    if (rename(fn,bf) == 0) 
                    {
                        fprintf(stderr,
                                "\nBack Off! I just backed up %s to %s\n",
                                fn,bf);
                    }
                    else
                    {
                        fprintf(stderr,"Sorry, I couldn't backup %s to %s\n",
                                fn,bf);
                    }
                    sfree(bf);
                }
#endif
            }
            else 
            {
                /* Check whether file exists */
                if (!gmx_fexist(fn))
                {
                    gmx_open(fn);
                }
            }
            snew(fio->xdr,1);
            xdrid = xdropen(fio->xdr,fn,newmode); 
            if (xdrid == 0)
            {
                if(newmode[0]=='r') 
                    gmx_fatal(FARGS,"Cannot open file %s for reading\nCheck permissions if it exists.",fn); 
                else
                    gmx_fatal(FARGS,"Cannot open file %s for writing.\nCheck your permissions, disk space and/or quota.",fn);
            }
            fio->fp = xdr_get_fp(xdrid);
        }
        else
        {
            /* If it is not, open it as a regular file */
            fio->fp = ffopen(fn,newmode);
        }
    }
    else
    {
        /* Use stdin/stdout for I/O */
        fio->iFTP   = efTPA;
        fio->fp     = bRead ? stdin : stdout;
        fio->fn     = strdup("STDIO");
        fio->bStdio = TRUE;
    }
    fio->bRead  = bRead;
    fio->bDouble= (sizeof(real) == sizeof(double));
    fio->bDebug = FALSE;
    fio->bOpen  = TRUE;
    fio->bLargerThan_off_t = FALSE;

#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return nfio;
}


/* this function may be called from a function that locks the fio_mutex, 
   which is why it exists in the first place. */
static int gmx_fio_close_lock(int fio, bool do_lock)
{
    int rc = 0;

#ifdef GMX_THREADS
    if (do_lock)
        tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);

    if (in_ftpset(FIO[fio].iFTP,asize(ftpXDR),ftpXDR)) {
        rc = !xdrclose(FIO[fio].xdr); /* xdrclose returns 1 if happy, 
                                         negate it */
        sfree(FIO[fio].xdr);
    }
    else {
        /* Don't close stdin and stdout! */
        if (!FIO[fio].bStdio)
            rc = fclose(FIO[fio].fp); /* fclose returns 0 if happy */
    }

    sfree(FIO[fio].fn);
    FIO[fio].bOpen = FALSE;
    do_read  = do_dummy;
    do_write = do_dummy;
#ifdef GMX_THREADS
    if (do_lock)
        tMPI_Thread_mutex_unlock(&fio_mutex);
#endif

    return rc;
}

int gmx_fio_close(int fio)
{
    return gmx_fio_close_lock(fio, TRUE);
}

FILE * gmx_fio_fopen(const char *fn,const char *mode)
{
    FILE *fp,*ret;
    int   fd;

    fd = gmx_fio_open(fn,mode);
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    ret=FIO[fd].fp;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return ret;
}


int gmx_fio_fclose(FILE *fp)
{
    int i,rc,found;

#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    found = 0;
    rc = -1;

    for(i=0;i<nFIO && !found;i++)
    {
        if(fp == FIO[i].fp)
        {
            rc = gmx_fio_close_lock(i,FALSE);
            found=1;
        }
    }
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return rc;
}


/* The fio_mutex should ALWAYS be locked when this function is called */
static int gmx_fio_get_file_position(int fio, off_t *offset)
{
    char buf[STRLEN];

    /* Flush the file, so we are sure it is written */
    if (gmx_fio_flush_lock(fio,FALSE))
    {
        char buf[STRLEN];
        sprintf(buf,"Cannot write file '%s'; maybe you are out of disk space or quota?",FIO[fio].fn);
        gmx_file(buf);
    }

    /* We cannot count on XDR being able to write 64-bit integers, 
       so separate into high/low 32-bit values.
       In case the filesystem has 128-bit offsets we only care 
       about the first 64 bits - we'll have to fix
       this when exabyte-size output files are common...
       */
#ifdef HAVE_FSEEKO
    *offset = ftello(FIO[fio].fp);
#else
    *offset = ftell(FIO[fio].fp);
#endif

    return 0;
}


int gmx_fio_check_file_position(int fio)
{
    /* If off_t is 4 bytes we can not store file offset > 2 GB.
     * If we do not have ftello, we will play it safe.
     */
#if (SIZEOF_OFF_T == 4 || !defined HAVE_FSEEKO)
    off_t offset;
    
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_get_file_position(fio,&offset);
    /* We have a 4 byte offset,
     * make sure that we will detect out of range for all possible cases.
     */
    if (offset < 0 || offset > 2147483647)
    {
        FIO[fio].bLargerThan_off_t = TRUE;
    }
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
#endif

    return 0;
}


int gmx_fio_get_output_file_positions(gmx_file_position_t **p_outputfiles, 
                                      int *p_nfiles)
{
    int                      i,nfiles,rc,nalloc;
    int                      pos_hi,pos_lo;
    long                     pos;	
    gmx_file_position_t *    outputfiles;
    char                     buf[STRLEN];

#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    nfiles = 0;

    nalloc = 100;
    snew(outputfiles,nalloc);

    for(i=0;i<nFIO;i++)
    {
        /* Skip the checkpoint files themselves, since they could be open when we call this routine... */
        if(FIO[i].bOpen && !FIO[i].bRead && !FIO[i].bStdio && 
                FIO[i].iFTP!=efCPT)
        {
            int ret;
            /* This is an output file currently open for writing, add it */
            if(nfiles == nalloc)
            {
                nalloc += 100;
                srenew(outputfiles,nalloc);
            }

            strncpy(outputfiles[nfiles].filename,FIO[i].fn,STRLEN-1);

            /* Get the file position */
            if (FIO[i].bLargerThan_off_t)
            {
                /* -1 signals out of range */
                outputfiles[nfiles].offset = -1;
            }
            else
            {
                gmx_fio_get_file_position(i,&outputfiles[nfiles].offset);
            }
            
            nfiles++;
        }
    }
    *p_nfiles = nfiles;
    *p_outputfiles = outputfiles;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif

    return 0;
}


void gmx_fio_select(int fio)
{
#ifdef DEBUG
    fprintf(stderr,"Select fio called with type %d for file %s\n",
            FIO[fio].iFTP,FIO[fio].fn);
#endif

#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    if (in_ftpset(FIO[fio].iFTP,asize(ftpXDR),ftpXDR)) {
#ifdef USE_XDR    
        do_read  = do_xdr;
        do_write = do_xdr;
#else
        gmx_fatal(FARGS,"Sorry, no XDR");
#endif
    }
    else if (in_ftpset(FIO[fio].iFTP,asize(ftpASC),ftpASC)) {
        do_read  = do_ascread;
        do_write = do_ascwrite;
    }
    else if (in_ftpset(FIO[fio].iFTP,asize(ftpBIN),ftpBIN)) {
        do_read  = do_binread;
        do_write = do_binwrite;
    }
#ifdef HAVE_XMl
    else if (in_ftpset(FIO[fio].iFTP,asize(ftpXML),ftpXML)) {
        do_read  = do_dummy;
        do_write = do_dummy;
    }
#endif
    else 
        gmx_fatal(FARGS,"Can not read/write topologies to file type %s",
                ftp2ext(curfio->iFTP));

#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    curfio = &(FIO[fio]);
}

void gmx_fio_setprecision(int fio,bool bDouble)
{
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    FIO[fio].bDouble = bDouble;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
}

bool gmx_fio_getdebug(int fio)
{
    bool ret;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    ret=FIO[fio].bDebug;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return FIO[fio].bDebug;
}

void gmx_fio_setdebug(int fio,bool bDebug)
{
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    FIO[fio].bDebug = bDebug;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
}

char *gmx_fio_getname(int fio)
{
    char *ret;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    ret=curfio->fn;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return ret;
}

void gmx_fio_setftp(int fio,int ftp)
{
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    FIO[fio].iFTP = ftp;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
}

int gmx_fio_getftp(int fio)
{
    int ret;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    ret=FIO[fio].iFTP;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return ret;
}

void gmx_fio_rewind(int fio)
{
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    if (FIO[fio].xdr) {
        xdrclose(FIO[fio].xdr);
        /* File is always opened as binary by xdropen */
        xdropen(FIO[fio].xdr,FIO[fio].fn,FIO[fio].bRead ? "r" : "w");
    }
    else
        frewind(FIO[fio].fp);
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
}

static int gmx_fio_flush_lock(int fio, bool do_lock)
{
    int rc=0;

#ifdef GMX_THREADS
    if (do_lock)
        tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    if (FIO[fio].fp)
        rc = fflush(FIO[fio].fp);
    else if (FIO[fio].xdr)
        rc = fflush ((FILE *) FIO[fio].xdr->x_private);
#ifdef GMX_THREADS
    if (do_lock)
        tMPI_Thread_mutex_unlock(&fio_mutex);
#endif

    return rc;
}

int gmx_fio_flush(int fio)
{
    return gmx_fio_flush_lock(fio, TRUE);
}

off_t gmx_fio_ftell(int fio)
{
    off_t ret=0;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    if (FIO[fio].fp)
        ret=ftell(FIO[fio].fp);
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return ret;
}

void gmx_fio_seek(int fio, off_t fpos)
{
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    if (FIO[fio].fp)
    {
#ifdef HAVE_FSEEKO
        fseeko(FIO[fio].fp,fpos,SEEK_SET);
#else
        fseek(FIO[fio].fp,fpos,SEEK_SET);
#endif
    }
    else
        gmx_file(FIO[fio].fn);
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
}

FILE *gmx_fio_getfp(int fio)
{
    FILE *ret=NULL;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    if (FIO[fio].fp)
        ret=FIO[fio].fp;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return ret;
}

XDR *gmx_fio_getxdr(int fio)
{
    XDR *ret=NULL;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    if (FIO[fio].xdr) 
        ret= FIO[fio].xdr;

#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return ret;
}

bool gmx_fio_getread(int fio)
{
    bool ret;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_lock(&fio_mutex);
#endif
    gmx_fio_check(fio);
    ret=FIO[fio].bRead;
#ifdef GMX_THREADS
    tMPI_Thread_mutex_unlock(&fio_mutex);
#endif
    return ret;
}

int xtc_seek_frame(int frame, int fio, int natoms)
{
    return xdr_xtc_seek_frame(frame,FIO[fio].fp,FIO[fio].xdr,natoms);
}

int xtc_seek_time(real time, int fio, int natoms)
{
    return xdr_xtc_seek_time(time,FIO[fio].fp,FIO[fio].xdr,natoms);
}
