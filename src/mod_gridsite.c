/*
   Copyright (c) 2003-5, Andrew McNab and Shiv Kaushal, 
   University of Manchester. All rights reserved.

   Redistribution and use in source and binary forms, with or
   without modification, are permitted provided that the following
   conditions are met:

     o Redistributions of source code must retain the above
       copyright notice, this list of conditions and the following
       disclaimer. 
     o Redistributions in binary form must reproduce the above
       copyright notice, this list of conditions and the following
       disclaimer in the documentation and/or other materials
       provided with the distribution. 

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/*------------------------------------------------------------------*
 * This program is part of GridSite: http://www.gridsite.org/       *
 *------------------------------------------------------------------*/

#ifndef VERSION
#define VERSION "x.x.x"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <apr_strings.h>

#include <ap_config.h>
#include <httpd.h>
#include <http_config.h>
#include <http_core.h>
#include <http_log.h>
#include <http_protocol.h>
#include <http_request.h>
#include <unixd.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>              
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>

#include <libxml/parser.h>
#include <libxml/tree.h>


#include "mod_ssl-private.h"

#include "gridsite.h"

#ifndef UNSET
#define UNSET -1
#endif

module AP_MODULE_DECLARE_DATA gridsite_module;

typedef struct
{
   char			*onetimesdir;
}  mod_gridsite_srv_cfg; /* per-server config choices */

typedef struct
{
   int			auth;
   int			envs;
   int			format;
   int			indexes;
   char			*indexheader;
   int			gridsitelink;
   char			*adminfile;
   char			*adminuri;
   char			*helpuri;
   char			*dnlists;
   char			*dnlistsuri;
   char			*adminlist;
   int			gsiproxylimit;
   char			*unzip;
   char			*methods;
   char			*editable;
   char			*headfile;
   char			*footfile;
   int			gridhttp;
   int			soap2cgi;
   char			*aclformat;
   char			*execmethod;
   ap_unix_identity_t	execugid;
   apr_fileperms_t	diskmode;
}  mod_gridsite_dir_cfg; /* per-directory config choices */

typedef struct
{
  xmlDocPtr doc;
//  char *outbuffer;
} soap2cgi_ctx; /* store per-request context for Soap2cgi in/out filters */

static const char Soap2cgiFilterName[]="Soap2cgiFilter";

static void mod_gridsite_soap2cgi_insert(request_rec *r)
{
    mod_gridsite_dir_cfg *conf;
    soap2cgi_ctx     *ctx;
    
    conf = (mod_gridsite_dir_cfg *) ap_get_module_config(r->per_dir_config,
                                                      &gridsite_module);
                                                      
    if (conf->soap2cgi) 
      {
        ctx = (soap2cgi_ctx *) malloc(sizeof(soap2cgi_ctx));        
        ctx->doc = NULL;
        
        ap_add_output_filter(Soap2cgiFilterName, ctx, r, r->connection);

        ap_add_input_filter(Soap2cgiFilterName, NULL, r, r->connection);
      }
}

xmlNodePtr find_one_child(xmlNodePtr parent_node, char *name)
{
    xmlNodePtr cur;

    for (cur = parent_node->children; cur != NULL; cur = cur->next)
       {
         if ((cur->type == XML_ELEMENT_NODE) &&
             (strcmp(cur->name, name) == 0)) return cur;
       }

    return NULL;
}

int add_one_node(xmlDocPtr doc, char *line)
{
    char *p, *name, *aftername, *attrname = NULL, *value = NULL;
    xmlNodePtr cur, cur_child;

    cur = xmlDocGetRootElement(doc);

    p = index(line, '=');
    if (p == NULL) return 1;

    *p = '\0';
    value = &p[1];

    name = line;

    while (1) /* go through each .-deliminated segment of line[] */
         {
           if ((p = index(name, '.')) != NULL)
             {
               *p = '\0';
               aftername = &p[1];
             }
           else aftername = &name[strlen(name)];

           if ((p = index(name, '_')) != NULL)
             {
               *p = '\0';
               attrname = &p[1];
             }

           cur_child = find_one_child(cur, name);

           if (cur_child == NULL)
                    cur_child = xmlNewChild(cur, NULL, name, NULL);

           cur = cur_child;

           name = aftername;

           if (attrname != NULL)
             {
               xmlSetProp(cur, attrname, value);
               return 0;
             }

           if (*name == '\0')
             {
               xmlNodeSetContent(cur, value);
               return 0;
             }             
         }
}

static apr_status_t mod_gridsite_soap2cgi_out(ap_filter_t *f,
                                              apr_bucket_brigade *bbIn)
{
    char        *p, *name, *outbuffer;
    request_rec *r = f->r;
    conn_rec    *c = r->connection;
    apr_bucket         *bucketIn, *pbktEOS;
    apr_bucket_brigade *bbOut;

    const char *data;
    apr_size_t len;
    char *buf;
    apr_size_t n;
    apr_bucket *pbktOut;

    soap2cgi_ctx *ctx;
    xmlNodePtr   root_node = NULL;
    xmlBufferPtr buff;

    ctx = (soap2cgi_ctx *) f->ctx;

// LIBXML_TEST_VERSION;

    bbOut = apr_brigade_create(r->pool, c->bucket_alloc);

    if (ctx->doc == NULL)
      {
        ctx->doc = xmlNewDoc("1.0");
             
        root_node = xmlNewNode(NULL, "Envelope");
        xmlDocSetRootElement(ctx->doc, root_node);
                                                                                
        xmlNewChild(root_node, NULL, "Header", NULL);
        xmlNewChild(root_node, NULL, "Body",   NULL);
      }
    
    apr_brigade_pflatten(bbIn, &outbuffer, &len, r->pool);
       
    /* split up buffer and feed each line to add_one_node() */
    
    name = outbuffer;
    
    while (*name != '\0')
         {
           p = index(name, '\n');
           if (p != NULL) 
             {
               *p = '\0';
               ++p;             
             }
           else p = &name[strlen(name)]; /* point to final NUL */
           
           add_one_node(ctx->doc, name);
           
           name = p;
         }

    APR_BRIGADE_FOREACH(bucketIn, bbIn)
       {
         if (APR_BUCKET_IS_EOS(bucketIn))
           {
             /* write out XML tree we have built */

             buff = xmlBufferCreate();
             xmlNodeDump(buff, ctx->doc, root_node, 0, 0);

// TODO: simplify/reduce number of copies or libxml vs APR buffers?

             buf = (char *) xmlBufferContent(buff);

             pbktOut = apr_bucket_heap_create(buf, strlen(buf), NULL, 
                                              c->bucket_alloc);

             APR_BRIGADE_INSERT_TAIL(bbOut, pbktOut);
       
             xmlBufferFree(buff);

             pbktEOS = apr_bucket_eos_create(c->bucket_alloc);
             APR_BRIGADE_INSERT_TAIL(bbOut, pbktEOS);

             continue;
           }
       }
       
    return ap_pass_brigade(f->next, bbOut);
}

static apr_status_t mod_gridsite_soap2cgi_in(ap_filter_t *f,
                                             apr_bucket_brigade *pbbOut,
                                             ap_input_mode_t eMode,
                                             apr_read_type_e eBlock,
                                             apr_off_t nBytes)
{
    request_rec *r = f->r;
    conn_rec *c = r->connection;
//    CaseFilterInContext *pCtx;
    apr_status_t ret;

#ifdef NEVERDEFINED

    ret = ap_get_brigade(f->next, pCtx->pbbTmp, eMode, eBlock, nBytes);    
 
    if (!(pCtx = f->ctx)) {
        f->ctx = pCtx = apr_palloc(r->pool, sizeof *pCtx);
        pCtx->pbbTmp = apr_brigade_create(r->pool, c->bucket_alloc);
    }
 
    if (APR_BRIGADE_EMPTY(pCtx->pbbTmp)) {
        ret = ap_get_brigade(f->next, pCtx->pbbTmp, eMode, eBlock, nBytes);
 
        if (eMode == AP_MODE_EATCRLF || ret != APR_SUCCESS)
            return ret;
    }
 
    while(!APR_BRIGADE_EMPTY(pCtx->pbbTmp)) {
        apr_bucket *pbktIn = APR_BRIGADE_FIRST(pCtx->pbbTmp);
        apr_bucket *pbktOut;
        const char *data;
        apr_size_t len;
        char *buf;
        int n;
 
        /* It is tempting to do this...
         * APR_BUCKET_REMOVE(pB);
         * APR_BRIGADE_INSERT_TAIL(pbbOut,pB);
         * and change the case of the bucket data, but that would be wrong
         * for a file or socket buffer, for example...
         */
                                                                                
        if(APR_BUCKET_IS_EOS(pbktIn)) {
            APR_BUCKET_REMOVE(pbktIn);
            APR_BRIGADE_INSERT_TAIL(pbbOut, pbktIn);
            break;
        }
                                                                                
        ret=apr_bucket_read(pbktIn, &data, &len, eBlock);
        if(ret != APR_SUCCESS)
            return ret;
                                                                                
        buf = malloc(len);
        for(n=0 ; n < len ; ++n)
            buf[n] = apr_toupper(data[n]);
                                                                                
        pbktOut = apr_bucket_heap_create(buf, len, 0, c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(pbbOut, pbktOut);
        apr_bucket_delete(pbktIn);
    }
#endif
                                                                                
    return APR_SUCCESS;
}

char *make_admin_footer(request_rec *r, mod_gridsite_dir_cfg *conf,
                        int isdirectory)
/*
    make string holding last modified text and admin links
*/
{
    char     *out, *https, *p, *dn = NULL, *file = NULL, *permstr = NULL, 
             *temp, modified[99], *dir_uri, *grst_cred_0 = NULL;
    GRSTgaclPerm  perm = GRST_PERM_NONE;
    struct tm mtime_tm;
    time_t    mtime_time;

    https = (char *) apr_table_get(r->subprocess_env, "HTTPS");

    dir_uri = apr_pstrdup(r->pool, r->uri);
    p = rindex(dir_uri, '/');

    if (p == NULL) return "";
    
    file = apr_pstrdup(r->pool, &p[1]);
    p[1] = '\0';
    /* dir_uri always gets both a leading and a trailing slash */
       
    out = apr_pstrdup(r->pool, "<p>\n");

    if (!isdirectory)
      {
        mtime_time = apr_time_sec(r->finfo.mtime);

        localtime_r(&mtime_time, &mtime_tm);
        strftime(modified, sizeof(modified), 
                 "%a&nbsp;%e&nbsp;%B&nbsp;%Y", &mtime_tm);    
        temp = apr_psprintf(r->pool,"<hr><small>Last modified %s\n", modified);
        out = apr_pstrcat(r->pool, out, temp, NULL);

        if ((conf->adminuri != NULL) &&
            (conf->adminuri[0] != '\0') &&
            (conf->adminfile != NULL) &&
            (conf->adminfile[0] != '\0') &&
            (strncmp(file, GRST_HIST_PREFIX, sizeof(GRST_HIST_PREFIX)-1) != 0))
          {
            temp = apr_psprintf(r->pool, 
                            ". <a href=\"%s?cmd=history&amp;file=%s\">"
                            "View&nbsp;page&nbsp;history</a>\n",
                            conf->adminfile, file);
            out = apr_pstrcat(r->pool, out, temp, NULL);
          }
          
        out = apr_pstrcat(r->pool, out, "</small>", NULL);
      }

    out = apr_pstrcat(r->pool, out, "<hr><small>", NULL);

    if (r->connection->notes != NULL)
         grst_cred_0 = (char *) 
                       apr_table_get(r->connection->notes, "GRST_CRED_0");

    if ((grst_cred_0 != NULL) && 
        (strncmp(grst_cred_0, "X509USER ", sizeof("X509USER")) == 0))
      {
         p = index(grst_cred_0, ' ');
         if (p != NULL)
           {
             p = index(++p, ' ');
             if (p != NULL)
               {
                 p = index(++p, ' ');
                 if (p != NULL)
                   {
                     p = index(++p, ' ');
                     if (p != NULL) dn = p;
                   }
               }
           }
      }
  
    if (dn != NULL) 
      {
        temp = apr_psprintf(r->pool, "You are %s<br>\n", dn);
        out = apr_pstrcat(r->pool, out, temp, NULL);
               
        if (r->notes != NULL)
                permstr = (char *) apr_table_get(r->notes, "GRST_PERM");

        if ((permstr != NULL) &&
            (conf->adminuri != NULL) &&
            (conf->adminuri[0] != '\0') &&
            (conf->adminfile != NULL) &&
            (conf->adminfile[0] != '\0'))
          {
            sscanf(permstr, "%d", &perm);
            
            if (!isdirectory &&
                GRSTgaclPermHasWrite(perm) &&
                (strncmp(file, GRST_HIST_PREFIX,
                         sizeof(GRST_HIST_PREFIX) - 1) != 0))
              {
                temp = apr_psprintf(r->pool, 
                     "<a href=\"%s?cmd=edit&amp;file=%s\">"
                     "Edit&nbsp;page</a> .\n", conf->adminfile, file);
                out = apr_pstrcat(r->pool, out, temp, NULL);
              }
                 
            if (GRSTgaclPermHasList(perm) || GRSTgaclPermHasWrite(perm))
              {
                temp = apr_psprintf(r->pool, 
                 "<a href=\"%s%s?cmd=managedir\">Manage&nbsp;directory</a> .\n",
                 dir_uri, conf->adminfile);

                out = apr_pstrcat(r->pool, out, temp, NULL);
              }                 
          }
      }
    
    if ((https != NULL) && (strcasecmp(https, "on") == 0))
         temp = apr_psprintf(r->pool,
                   "<a href=\"http://%s%s\">Switch&nbsp;to&nbsp;HTTP</a> \n", 
                   r->server->server_hostname, r->unparsed_uri);
    else temp = apr_psprintf(r->pool,
                   "<a href=\"https://%s%s\">Switch&nbsp;to&nbsp;HTTPS</a> \n",
                   r->server->server_hostname, r->unparsed_uri);
    
    out = apr_pstrcat(r->pool, out, temp, NULL);

    if ((conf->helpuri != NULL) && (conf->helpuri[0] != '\0'))
      {
        temp = apr_psprintf(r->pool,
                   ". <a href=\"%s\">Website&nbsp;Help</a>\n", conf->helpuri);
        out = apr_pstrcat(r->pool, out, temp, NULL);
      }

    if ((!isdirectory) &&
        (conf->adminuri != NULL) &&
        (conf->adminuri[0] != '\0') &&
        (conf->adminfile != NULL) &&
        (conf->adminfile[0] != '\0'))
      {
        temp = apr_psprintf(r->pool, ". <a href=\"%s?cmd=print&amp;file=%s\">"
               "Print&nbsp;View</a>\n", conf->adminfile, file);
        out = apr_pstrcat(r->pool, out, temp, NULL);
      }

    if (conf->gridsitelink)
      {
        temp = apr_psprintf(r->pool,
           ". Built with <a href=\"http://www.gridsite.org/\">"
           "GridSite</a>&nbsp;%s\n", VERSION);
        out = apr_pstrcat(r->pool, out, temp, NULL);
      }
                     
    out = apr_pstrcat(r->pool, out, "\n</small>\n", NULL);

    return out;
}

int html_format(request_rec *r, mod_gridsite_dir_cfg *conf)
/* 
    try to do GridSite formatting of .html files (NOT .shtml etc)
*/
{
    int    i, fd, errstatus;
    char  *buf, *p, *file, *s, *head_formatted, *header_formatted,
          *body_formatted, *admin_formatted, *footer_formatted;
    size_t length;
    struct stat statbuf;
    apr_file_t *fp;
    
    if (r->finfo.filetype == APR_NOFILE) return HTTP_NOT_FOUND;
    
    if (apr_file_open(&fp, r->filename, APR_READ, 0, r->pool) != 0)
                                     return HTTP_INTERNAL_SERVER_ERROR;
        
    file = rindex(r->uri, '/');
    if (file != NULL) ++file; /* file points to name without path */

    buf = apr_palloc(r->pool, (size_t)(r->finfo.size + 1));
    length = r->finfo.size;
    apr_file_read(fp, buf, &length);
    buf[r->finfo.size] = '\0';
    apr_file_close(fp);

    /* **** try to find a header file in this or parent directories **** */

    /* first make a buffer big enough to hold path names we want to try */
    fd = -1;
    s = malloc(strlen(r->filename) + strlen(conf->headfile) + 1);
    strcpy(s, r->filename);
    
    for (;;)
       {
         p = rindex(s, '/');
         if (p == NULL) break; /* failed to find one */
         p[1] = '\0';
         strcat(p, conf->headfile);

         fd = open(s, O_RDONLY);
         if (fd != -1) break; /* found one */

         *p = '\0';
       }
            
    free(s);

    if (fd == -1) /* not found, so set up not to output one */
      {
        head_formatted   = apr_pstrdup(r->pool, "");
        header_formatted = apr_pstrdup(r->pool, "");
        body_formatted   = buf;
      }
    else /* found a header file, so set up head and body to surround it */
      {
        fstat(fd, &statbuf);
        header_formatted = apr_palloc(r->pool, statbuf.st_size + 1);
        read(fd, header_formatted, statbuf.st_size);
        header_formatted[statbuf.st_size] = '\0';
        close(fd);

        p = strstr(buf, "<body");
        if (p == NULL) p = strstr(buf, "<BODY");
        if (p == NULL) p = strstr(buf, "<Body");

        if (p == NULL) 
          {
            head_formatted = apr_pstrdup(r->pool, "");
            body_formatted = buf;
          }
        else
          {
            *p = '\0';
            head_formatted = buf;
            ++p;
        
            while ((*p != '>') && (*p != '\0')) ++p;

            if (*p == '\0')
              {
                body_formatted = p;
              }
            else
              {
                *p = '\0';
                ++p;
                body_formatted = p;
              }        
          }
      }

    /* **** remove closing </body> tag from body **** */

    p = strstr(body_formatted, "</body");
    if (p == NULL) p = strstr(body_formatted, "</BODY");
    if (p == NULL) p = strstr(body_formatted, "</Body");
    
    if (p != NULL) *p = '\0';

    /* **** set up dynamic part of footer to go at end of body **** */

    admin_formatted = make_admin_footer(r, conf, FALSE);
    
    /* **** try to find a footer file in this or parent directories **** */

    /* first make a buffer big enough to hold path names we want to try */
    fd = -1;
    s  = malloc(strlen(r->filename) + strlen(conf->footfile));
    strcpy(s, r->filename);
    
    for (;;)
       {
         p = rindex(s, '/');
         if (p == NULL) break; /* failed to find one */
    
         p[1] = '\0';
         strcat(p, conf->footfile);
    
         fd = open(s, O_RDONLY);
         if (fd != -1) break; /* found one */

         *p = '\0';
       }
            
    free(s);

    if (fd == -1) /* failed to find a footer, so set up empty default */
      { 
        footer_formatted = apr_pstrdup(r->pool, "");
      }
    else /* found a footer, so set up to use it */
      {
        fstat(fd, &statbuf);
        footer_formatted = apr_palloc(r->pool, statbuf.st_size + 1);
        read(fd, footer_formatted, statbuf.st_size);
        footer_formatted[statbuf.st_size] = '\0';
        close(fd);
      }

    /* **** can now calculate the Content-Length and output headers **** */
      
    length = strlen(head_formatted) + strlen(header_formatted) + 
             strlen(body_formatted) + strlen(admin_formatted)  +
             strlen(footer_formatted);

    ap_set_content_length(r, length);
    ap_set_content_type(r, "text/html");

    /* ** output the HTTP body (HTML Head+Body) ** */

    ap_rputs(head_formatted,   r);
    ap_rputs(header_formatted, r);
    ap_rputs(body_formatted,   r);
    ap_rputs(admin_formatted,  r);
    ap_rputs(footer_formatted, r);

    return OK;
}

int html_dir_list(request_rec *r, mod_gridsite_dir_cfg *conf)
/* 
    output HTML directory listing, with level of formatting controlled
    by GridSiteHtmlFormat/conf->format
*/
{
    int    i, fd, n;
    char  *buf, *p, *s, *head_formatted, *header_formatted,
          *body_formatted, *admin_formatted, *footer_formatted, *temp,
           modified[99], *d_namepath, *indexheaderpath, *indexheadertext;
    size_t length;
    struct stat statbuf;
    struct tm   mtime_tm;
    struct dirent **namelist;
    
    if (r->finfo.filetype == APR_NOFILE) return HTTP_NOT_FOUND;
        
    head_formatted = apr_psprintf(r->pool, 
      "<head><title>Directory listing %s</title></head>\n", r->uri);

    if (conf->format)
      {
        /* **** try to find a header file in this or parent directories **** */

        /* first make a buffer big enough to hold path names we want to try */
        fd = -1;
        s = malloc(strlen(r->filename) + strlen(conf->headfile) + 1);
        strcpy(s, r->filename);
    
        for (;;)
           {
             p = rindex(s, '/');
             if (p == NULL) break; /* failed to find one */
             p[1] = '\0';
             strcat(p, conf->headfile);
    
             fd = open(s, O_RDONLY);
             if (fd != -1) break; /* found one */

             *p = '\0';
           }
            
        free(s);

        if (fd == -1) /* not found, so set up to output sensible default */
          {
            header_formatted = apr_pstrdup(r->pool, "<body bgcolor=white>");
          }
        else /* found a header file, so set up head and body to surround it */
          {
            fstat(fd, &statbuf);
            header_formatted = apr_palloc(r->pool, statbuf.st_size + 1);
            read(fd, header_formatted, statbuf.st_size);
            header_formatted[statbuf.st_size] = '\0';
            close(fd);
          }
      }
    else header_formatted = apr_pstrdup(r->pool, "<body bgcolor=white>");
            
    body_formatted = apr_psprintf(r->pool, 
      "<h1>Directory listing %s</h1>\n", r->uri);
      
    if (conf->indexheader != NULL)
      {
        indexheaderpath = apr_psprintf(r->pool, "%s/%s", r->filename, 
                                                         conf->indexheader);
        fd = open(indexheaderpath, O_RDONLY);
        if (fd != -1)
          {
            fstat(fd, &statbuf);
            indexheadertext = apr_palloc(r->pool, statbuf.st_size + 1);
            read(fd, indexheadertext, statbuf.st_size);
            indexheadertext[statbuf.st_size] = '\0';
            close(fd);
            
            body_formatted = apr_pstrcat(r->pool, body_formatted,
                                         indexheadertext, NULL);
          }
      }

    body_formatted = apr_pstrcat(r->pool, body_formatted, "<p><table>\n", NULL);

    if (r->unparsed_uri[1] != '\0')
     body_formatted = apr_pstrcat(r->pool, body_formatted, 
        "<tr><td colspan=3>[<a href=\"../\">Parent directory</a>]</td></tr>\n", 
         NULL);
      
    n = scandir(r->filename, &namelist, 0, versionsort);
    while (n--)
         {
           if ((namelist[n]->d_name[0] != '.') && 
               ((conf->indexheader == NULL) || 
                (strcmp(conf->indexheader, namelist[n]->d_name) != 0)))
             {
               d_namepath = apr_psprintf(r->pool, "%s/%s", r->filename,
                                                  namelist[n]->d_name);
               stat(d_namepath, &statbuf);
               
               localtime_r(&(statbuf.st_mtime), &mtime_tm);
               strftime(modified, sizeof(modified), 
              "<td align=right>%R</td><td align=right>%e&nbsp;%b&nbsp;%y</td>",
                        &mtime_tm);    
                              
               if (S_ISDIR(statbuf.st_mode))
                    temp = apr_psprintf(r->pool, 
                      "<tr><td><a href=\"%s/\" content-length=\"%ld\" "
                      "last-modified=\"%ld\">"
                      "%s/</a></td>"
                      "<td align=right>%ld</td>%s</tr>\n", 
                      namelist[n]->d_name, statbuf.st_size, statbuf.st_mtime,
                      namelist[n]->d_name, 
                      statbuf.st_size, modified);
               else temp = apr_psprintf(r->pool, 
                      "<tr><td><a href=\"%s\" content-length=\"%ld\" "
                      "last-modified=\"%ld\">"
                      "%s</a></td>"
                      "<td align=right>%ld</td>%s</tr>\n", 
                      namelist[n]->d_name, statbuf.st_size, statbuf.st_mtime,
                      namelist[n]->d_name, 
                      statbuf.st_size, modified);

               body_formatted = apr_pstrcat(r->pool,body_formatted,temp,NULL);
             }

           free(namelist[n]);
         }
                 
    free(namelist);
    
    body_formatted = apr_pstrcat(r->pool, body_formatted, "</table>\n", NULL);

    if (conf->format)
      {
        /* **** set up dynamic part of footer to go at end of body **** */

        admin_formatted = make_admin_footer(r, conf, TRUE);
    
        /* **** try to find a footer file in this or parent directories **** */

        /* first make a buffer big enough to hold path names we want to try */
        fd = -1;
        s  = malloc(strlen(r->filename) + strlen(conf->footfile));
        strcpy(s, r->filename);
    
        for (;;)
           {
             p = rindex(s, '/');
             if (p == NULL) break; /* failed to find one */
    
             p[1] = '\0';
             strcat(p, conf->footfile);
    
             fd = open(s, O_RDONLY);
             if (fd != -1) break; /* found one */

             *p = '\0';
           }
            
        free(s);

        if (fd == -1) /* failed to find a footer, so use standard default */
          {
            footer_formatted = apr_pstrdup(r->pool, "</body>");
          }
        else /* found a footer, so set up to use it */
          {
            fstat(fd, &statbuf);
            footer_formatted = apr_palloc(r->pool, statbuf.st_size + 1);
            read(fd, footer_formatted, statbuf.st_size);
            footer_formatted[statbuf.st_size] = '\0';
            close(fd);
          }
      }
    else
      {
        admin_formatted = apr_pstrdup(r->pool, "");
        footer_formatted = apr_pstrdup(r->pool, "</body>");
      }

    /* **** can now calculate the Content-Length and output headers **** */
      
    length = strlen(head_formatted) + strlen(header_formatted) + 
             strlen(body_formatted) + strlen(admin_formatted)  +
             strlen(footer_formatted);

    ap_set_content_length(r, length);
    ap_set_content_type(r, "text/html");

    /* ** output the HTTP body (HTML Head+Body) ** */

    ap_rputs(head_formatted,   r);
    ap_rputs(header_formatted, r);
    ap_rputs(body_formatted,   r);
    ap_rputs(admin_formatted,  r);
    ap_rputs(footer_formatted, r);

    return OK;
}

int http_gridhttp(request_rec *r, mod_gridsite_dir_cfg *conf)
{ 
    int          i;
    char        *httpurl, *filetemplate, *cookievalue, *envname_i, 
                *grst_cred_i, expires_str[APR_RFC822_DATE_LEN];
    apr_uint64_t gridauthcookie;
    apr_table_t *env;
    apr_time_t   expires_time;
    apr_file_t  *fp;

    /* create random cookie and gridauthcookie file */

    if (apr_generate_random_bytes((char *) &gridauthcookie, 
                                  sizeof(gridauthcookie))
         != APR_SUCCESS) return HTTP_INTERNAL_SERVER_ERROR;
    
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
               "Generated GridHTTP onetime passcode %016llx", gridauthcookie);

    filetemplate = apr_psprintf(r->pool, "%s/%016llxXXXXXX", 
     ap_server_root_relative(r->pool,
      ((mod_gridsite_srv_cfg *) 
       ap_get_module_config(r->server->module_config, 
                                    &gridsite_module))->onetimesdir),
     gridauthcookie);

    if (apr_file_mktemp(&fp, 
                        filetemplate, 
                        APR_CREATE | APR_WRITE | APR_EXCL,
                        r->pool)
                      != APR_SUCCESS) return HTTP_INTERNAL_SERVER_ERROR;
                                    
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
               "Created passcode file %s", filetemplate);

    expires_time = apr_time_now() + apr_time_from_sec(300); 
    /* onetime cookies are valid for only 5 mins! */

    apr_file_printf(fp, 
                   "expires=%lu\ndomain=%s\npath=%s\nonetime=yes\nmethod=%s\n", 
                   (time_t) apr_time_sec(expires_time),
                   r->hostname, r->uri, r->method);
    /* above variables are evaluated in order and method= MUST be last! */

    for (i=0; ; ++i)
       {
         envname_i = apr_psprintf(r->pool, "GRST_CRED_%d", i);
         if (grst_cred_i = (char *)
                           apr_table_get(r->connection->notes, envname_i))
           {
             apr_file_printf(fp, "%s=%s\n", envname_i, grst_cred_i);
           }
         else break; /* GRST_CRED_i are numbered consecutively */
       }

    if (apr_file_close(fp) != APR_SUCCESS) 
      {
        apr_file_remove(filetemplate, r->pool); /* try to clean up */
        return HTTP_INTERNAL_SERVER_ERROR;
      }
    
    /* send redirection header back to client */
       
    cookievalue = rindex(filetemplate, '/');
    if (cookievalue != NULL) ++cookievalue;
    else cookievalue = filetemplate;
       
    apr_rfc822_date(expires_str, expires_time);

    apr_table_add(r->headers_out, 
                  apr_pstrdup(r->pool, "Set-Cookie"), 
                  apr_psprintf(r->pool, 
                  "GRIDHTTP_ONETIME=%s; "
                  "expires=%s; "
                  "domain=%s; "
                  "path=%s",
                  cookievalue, expires_str, r->hostname, r->uri));
    
    httpurl = apr_pstrcat(r->pool, "http://", r->hostname,
                        ap_escape_uri(r->pool, r->uri), NULL);
    apr_table_setn(r->headers_out, apr_pstrdup(r->pool, "Location"), httpurl);

    r->status = HTTP_MOVED_TEMPORARILY;  
    return OK;
}

int http_put_method(request_rec *r, mod_gridsite_dir_cfg *conf)
{
  char        buf[2048];
  size_t      length;
  int         retcode;
  apr_file_t *fp;
    
  /* ***  check if directory creation: PUT /.../  *** */

  if ((r->unparsed_uri    != NULL) && 
      (r->unparsed_uri[0] != '\0') &&
      (r->unparsed_uri[strlen(r->unparsed_uri) - 1] == '/'))
    {
      if (apr_dir_make(r->filename, 
                       conf->diskmode 
                       | APR_UEXECUTE | APR_GEXECUTE | APR_WEXECUTE, 
                       r->pool) != 0) return HTTP_INTERNAL_SERVER_ERROR;

      /* we force the permissions, rather than accept any existing ones */

      apr_file_perms_set(r->filename, conf->diskmode
                             | APR_UEXECUTE | APR_GEXECUTE | APR_WEXECUTE);
                             
      ap_set_content_length(r, 0);
      ap_set_content_type(r, "text/html");
      return OK;
    }

  /* ***  otherwise assume trying to create a regular file *** */

  if (apr_file_open(&fp, r->filename, 
      APR_WRITE | APR_CREATE | APR_BUFFERED | APR_TRUNCATE,
      conf->diskmode, r->pool) != 0) return HTTP_INTERNAL_SERVER_ERROR;
   
  /* we force the permissions, rather than accept any existing ones */

  apr_file_perms_set(r->filename, conf->diskmode);
                             
// TODO: need to add Range: support at some point too
// Also return 201 Created rather than 200 OK if not already existing

  retcode = ap_setup_client_block(r, REQUEST_CHUNKED_DECHUNK);
  if (retcode == OK)
    {
      if (ap_should_client_block(r))
        while ((length = ap_get_client_block(r, buf, sizeof(buf))) > 0)
               if (apr_file_write(fp, buf, &length) != 0) 
                 {
                   retcode = HTTP_INTERNAL_SERVER_ERROR;
                   break;
                 }

      ap_set_content_length(r, 0);
      ap_set_content_type(r, "text/html");
    }

  if (apr_file_close(fp) != 0) return HTTP_INTERNAL_SERVER_ERROR;

  return retcode;
}

int http_delete_method(request_rec *r, mod_gridsite_dir_cfg *conf)
{
  if (apr_file_remove(r->filename, r->pool) != 0) return HTTP_FORBIDDEN;
       
  ap_set_content_length(r, 0);
  ap_set_content_type(r, "text/html");

  return OK;
}

int http_move_method(request_rec *r, mod_gridsite_dir_cfg *conf)
{
  char *destination_translated = NULL;
  
  if (r->notes != NULL) destination_translated = 
            (char *) apr_table_get(r->notes, "GRST_DESTINATION_TRANSLATED");


  if ((destination_translated == NULL) ||  
      (apr_file_rename(r->filename, destination_translated, r->pool) != 0))
                                                       return HTTP_FORBIDDEN;

  ap_set_content_length(r, 0);
  ap_set_content_type(r, "text/html");

  return OK;
}

static int mod_gridsite_dir_handler(request_rec *r, mod_gridsite_dir_cfg *conf)
/*
   handler switch for directories
*/
{
    /* *** is this a write method? only possible if  GridSiteAuth on *** */

    if (conf->auth)
      {
        if ((r->method_number == M_PUT) && 
            (conf->methods != NULL) &&
            (strstr(conf->methods, " PUT "   ) != NULL))
                                           return http_put_method(r, conf);

        if ((r->method_number == M_DELETE) &&
            (conf->methods != NULL) &&
            (strstr(conf->methods, " DELETE ") != NULL)) 
                                           return http_delete_method(r, conf);
      }
      
    /* *** directory listing? *** */
    
    if ((r->method_number == M_GET) && (conf->indexes))       
                       return html_dir_list(r, conf); /* directory listing */
    
    return DECLINED; /* *** nothing to see here, move along *** */
}

static int mod_gridsite_nondir_handler(request_rec *r, mod_gridsite_dir_cfg *conf)
/*
   one big handler switch for everything other than directories, since we 
   might be responding to MIME * / * for local PUT, MOVE, COPY and DELETE, 
   and GET inside ghost directories.
*/
{
    char *upgradeheader, *upgradespaced, *p;
    const char *https_env;

    /* *** is this a write method or GridHTTP HTTPS->HTTP redirection? 
           only possible if  GridSiteAuth on *** */
    
    if (conf->auth)
      {
        if ((conf->gridhttp) &&
            ((r->method_number == M_GET) || 
             ((r->method_number == M_PUT) && 
              (strstr(conf->methods, " PUT ") != NULL))) &&
            ((upgradeheader = (char *) apr_table_get(r->headers_in,
                                                     "Upgrade")) != NULL) &&
            ((https_env=apr_table_get(r->subprocess_env,"HTTPS")) != NULL) &&
            (strcasecmp(https_env, "on") == 0))
          {
            upgradespaced = apr_psprintf(r->pool, " %s ", upgradeheader);

            for (p=upgradespaced; *p != '\0'; ++p)
             if ((*p == ',') || (*p == '\t')) *p = ' ';

// TODO: what if we're pointing at a CGI or some dynamic content???
 
            if (strstr(upgradespaced, " GridHTTP/1.0 ") != NULL)
                                            return http_gridhttp(r, conf);
          }

        if ((r->method_number == M_PUT) && 
            (conf->methods != NULL) &&
            (strstr(conf->methods, " PUT "   ) != NULL))
                                           return http_put_method(r, conf);

        if ((r->method_number == M_DELETE) &&
            (conf->methods != NULL) &&
            (strstr(conf->methods, " DELETE ") != NULL)) 
                                           return http_delete_method(r, conf);

        if ((r->method_number == M_MOVE) &&
            (conf->methods != NULL) &&
            (strstr(conf->methods, " MOVE ") != NULL)) 
                                           return http_move_method(r, conf);
      }

    /* *** check if a special ghost admin CGI *** */
      
    if (conf->adminfile && conf->adminuri &&
        (strlen(r->filename) > strlen(conf->adminfile) + 1) &&
        (strcmp(&(r->filename[strlen(r->filename) - strlen(conf->adminfile)]),
                                                    conf->adminfile) == 0) &&
        (r->filename[strlen(r->filename)-strlen(conf->adminfile)-1] == '/') &&
        ((r->method_number == M_POST) ||
         (r->method_number == M_GET))) 
      {
        ap_internal_redirect(conf->adminuri, r);
        return OK;
      }
      
    /* *** finally look for .html files that we should format *** */

    if ((conf->format) &&  /* conf->format set by  GridSiteHtmlFormat on */ 
        (strlen(r->filename) > 5) &&
        (strcmp(&(r->filename[strlen(r->filename)-5]), ".html") == 0) &&
        (r->method_number == M_GET)) return html_format(r, conf);
     
    return DECLINED; /* *** nothing to see here, move along *** */
}

static void recurse4dirlist(char *dirname, time_t *dirs_time,
                             char *fulluri, int fullurilen,
                             char *encfulluri, int enclen,
                             apr_pool_t *pool, char **body,
                             int recurse_level)
/* try to find DN Lists in dir[] and its subdirs that match the fulluri[]
   prefix. add blobs of HTML to body as they are found. */
{
   char          *unencname, modified[99], *oneline, *d_namepath;
   DIR           *oneDIR;
   struct dirent *onedirent;
   struct tm      mtime_tm;
   size_t         length;
   struct stat    statbuf;

   if ((stat(dirname, &statbuf) != 0) ||
       (!S_ISDIR(statbuf.st_mode)) ||
       ((oneDIR = opendir(dirname)) == NULL)) return;

   if (statbuf.st_mtime > *dirs_time) *dirs_time = statbuf.st_mtime;

   while ((onedirent = readdir(oneDIR)) != NULL)
        {
          if (onedirent->d_name[0] == '.') continue;
        
          d_namepath = apr_psprintf(pool, "%s/%s", dirname, onedirent->d_name);
          if (stat(d_namepath, &statbuf) != 0) continue;

          if (S_ISDIR(statbuf.st_mode) && (recurse_level < GRST_RECURS_LIMIT)) 
                 recurse4dirlist(d_namepath, dirs_time, fulluri,
                                 fullurilen, encfulluri, enclen, 
                                 pool, body, recurse_level + 1);
          else if ((strncmp(onedirent->d_name, encfulluri, enclen) == 0) &&
                   (onedirent->d_name[strlen(onedirent->d_name) - 1] != '~'))
            {
              unencname = GRSThttpUrlDecode(onedirent->d_name);
                    
              if (strncmp(unencname, fulluri, fullurilen) == 0)
                {

                  if (statbuf.st_mtime > *dirs_time) 
                                                *dirs_time = statbuf.st_mtime;

                  localtime_r(&(statbuf.st_mtime), &mtime_tm);
                  strftime(modified, sizeof(modified), 
              "<td align=right>%R</td><td align=right>%e&nbsp;%b&nbsp;%y</td>",
                       &mtime_tm);

                  oneline = apr_psprintf(pool,
                                     "<tr><td><a href=\"%s\" "
                                     "content-length=\"%ld\" "
                                     "last-modified=\"%ld\">"
                                     "%s</a></td>"
                                     "<td align=right>%ld</td>%s</tr>\n", 
                                     &unencname[fullurilen], statbuf.st_size, 
                                     statbuf.st_mtime, unencname, 
                                     statbuf.st_size, modified);

                  *body = apr_pstrcat(pool, *body, oneline, NULL);
                }      
                      
              free(unencname); /* libgridsite doesnt use pools */    
            }
        }
        
   closedir(oneDIR);
}

static int mod_gridsite_dnlistsuri_dir_handler(request_rec *r, 
                                               mod_gridsite_dir_cfg *conf)
/*
    virtual DN-list file lister: make all DN lists on the dn-lists
    path of this server appear to be in the dn-lists directory itself
    (ie where they appear in the DN lists path doesnt matter, as long
    as their name matches)
*/
{
    int            enclen, fullurilen, fd;
    char          *fulluri, *encfulluri, *dn_list_ptr, *dirname, *unencname,
                  *body, *oneline, *p, *s,
                  *head_formatted, *header_formatted, *footer_formatted,
                  *permstr = NULL;
    struct stat    statbuf;
    size_t         length;
    time_t         dirs_time = 0;
    GRSTgaclPerm   perm = GRST_PERM_NONE;
        
    if (r->notes != NULL)
           permstr = (char *) apr_table_get(r->notes, "GRST_PERM");

    if (permstr != NULL) sscanf(permstr, "%d", &perm);

    fulluri = apr_psprintf(r->pool, "https://%s%s",
                                    ap_get_server_name(r), conf->dnlistsuri);
    fullurilen = strlen(fulluri);

    encfulluri = GRSThttpUrlEncode(fulluri);
    enclen     = strlen(encfulluri);

    if (conf->dnlists != NULL) p = conf->dnlists;
    else p = getenv("GRST_DN_LISTS");

    if (p == NULL) p = GRST_DN_LISTS;
    dn_list_ptr = apr_pstrdup(r->pool, p);

    head_formatted = apr_psprintf(r->pool, 
      "<head><title>Directory listing %s</title></head>\n", r->uri);

    if (conf->format)
      {
        /* **** try to find a header file in this or parent directories **** */

        /* first make a buffer big enough to hold path names we want to try */
        fd = -1;
        s = malloc(strlen(r->filename) + strlen(conf->headfile) + 1);
        strcpy(s, r->filename);
    
        for (;;)
           {
             p = rindex(s, '/');
             if (p == NULL) break; /* failed to find one */
             p[1] = '\0';
             strcat(p, conf->headfile);
    
             fd = open(s, O_RDONLY);
             if (fd != -1) break; /* found one */

             *p = '\0';
           }
            
        free(s);

        if (fd == -1) /* not found, so set up to output sensible default */
          {
            header_formatted = apr_pstrdup(r->pool, "<body bgcolor=white>");
          }
        else /* found a header file, so set up head and body to surround it */
          {
            fstat(fd, &statbuf);
            header_formatted = apr_palloc(r->pool, statbuf.st_size + 1);
            read(fd, header_formatted, statbuf.st_size);
            header_formatted[statbuf.st_size] = '\0';
            close(fd);
          }
      }
    else header_formatted = apr_pstrdup(r->pool, "<body bgcolor=white>");
            
    body = apr_psprintf(r->pool, 
      "<h1>Directory listing %s</h1>\n<table>", r->uri);

    if ((r->uri)[1] != '\0')
     body = apr_pstrcat(r->pool, body, 
       "<tr><td>[<a href=\"../\">Parent directory</a>]</td></tr>\n",
       NULL);

    while ((dirname = strsep(&dn_list_ptr, ":")) != NULL)
        recurse4dirlist(dirname, &dirs_time, fulluri, fullurilen,
                                 encfulluri, enclen, r->pool, &body, 0);

    if ((stat(r->filename, &statbuf) == 0) &&
        S_ISDIR(statbuf.st_mode) && 
        GRSTgaclPermHasWrite(perm))
      {
        oneline = apr_psprintf(r->pool,
           "<form action=\"%s%s\" method=post>\n"
           "<input type=hidden name=cmd value=managedir>"
           "<tr><td colspan=4 align=center><small><input type=submit "
           "value=\"Manage directory\"></small></td></tr></form>\n",
           r->uri, conf->adminfile);
          
        body = apr_pstrcat(r->pool, body, oneline, NULL);
      } 

    body = apr_pstrcat(r->pool, body, "</table>\n", NULL);

    free(encfulluri); /* libgridsite doesnt use pools */

    if (conf->format)
      {
        /* **** try to find a footer file in this or parent directories **** */

        /* first make a buffer big enough to hold path names we want to try */
        fd = -1;
        s  = malloc(strlen(r->filename) + strlen(conf->footfile));
        strcpy(s, r->filename);
    
        for (;;)
           {
             p = rindex(s, '/');
             if (p == NULL) break; /* failed to find one */
    
             p[1] = '\0';
             strcat(p, conf->footfile);
    
             fd = open(s, O_RDONLY);
             if (fd != -1) break; /* found one */

             *p = '\0';
           }
            
        free(s);

        if (fd == -1) /* failed to find a footer, so use standard default */
          {
            footer_formatted = apr_pstrdup(r->pool, "</body>");
          }
        else /* found a footer, so set up to use it */
          {
            fstat(fd, &statbuf);
            footer_formatted = apr_palloc(r->pool, statbuf.st_size + 1);
            read(fd, footer_formatted, statbuf.st_size);
            footer_formatted[statbuf.st_size] = '\0';
            close(fd);
          }
      }
    else footer_formatted = apr_pstrdup(r->pool, "</body>");

    /* **** can now calculate the Content-Length and output headers **** */
      
    length = strlen(head_formatted) + strlen(header_formatted) + 
             strlen(body) + strlen(footer_formatted);

    ap_set_content_length(r, length);
    r->mtime = apr_time_from_sec(dirs_time);
    ap_set_last_modified(r);
    ap_set_content_type(r, "text/html");

    /* ** output the HTTP body (HTML Head+Body) ** */
    ap_rputs(head_formatted,   r);
    ap_rputs(header_formatted, r);
    ap_rputs(body,		   r);
    ap_rputs(footer_formatted, r);

    return OK;
}

static char *recurse4file(char *dir, char *file, apr_pool_t *pool, 
                          int recurse_level)
/* try to find file[] in dir[]. try subdirs if not found.
   return full path to first found version or NULL on failure */
{
    char          *fullfilename, *fulldirname;
    struct stat    statbuf;
    DIR           *dirDIR;
    struct dirent *file_ent;

    /* try to find in current directory */

    fullfilename = apr_psprintf(pool, "%s/%s", dir, file);

    if (stat(fullfilename, &statbuf) == 0) return fullfilename;

    /* maybe search in subdirectories */

    if (recurse_level >= GRST_RECURS_LIMIT) return NULL;

    dirDIR = opendir(dir);

    if (dirDIR == NULL) return NULL;

    while ((file_ent = readdir(dirDIR)) != NULL)
       {
         if (file_ent->d_name[0] == '.') continue;

         fulldirname = apr_psprintf(pool, "%s/%s", dir, file_ent->d_name);
         if ((stat(fulldirname, &statbuf) == 0) &&
             S_ISDIR(statbuf.st_mode) &&
             ((fullfilename = recurse4file(fulldirname, file,
                                           pool, recurse_level + 1)) != NULL))
           {
             closedir(dirDIR);
             return fullfilename;
           }
       }

    closedir(dirDIR);

    return NULL;
}

static int mod_gridsite_dnlistsuri_handler(request_rec *r, 
                                           mod_gridsite_dir_cfg *conf)
/*
    virtual DN-list file generator
*/
{
    int          fd;
    char        *fulluri, *encfulluri, *dn_list_ptr, *filename, *dirname, *p,
                *buf;
    struct stat  statbuf;
    
    /* *** check if a special ghost admin CGI *** */
      
    if (conf->adminfile && conf->adminuri &&
        (strlen(r->filename) > strlen(conf->adminfile) + 1) &&
        (strcmp(&(r->filename[strlen(r->filename) - strlen(conf->adminfile)]),
                                                    conf->adminfile) == 0) &&
        (r->filename[strlen(r->filename)-strlen(conf->adminfile)-1] == '/') &&
        ((r->method_number == M_POST) ||
         (r->method_number == M_GET))) 
      {
        ap_internal_redirect(conf->adminuri, r);
        return OK;
      }

    fulluri = apr_psprintf(r->pool, "https://%s%s", 
                                    ap_get_server_name(r), r->uri);

    encfulluri = GRSThttpUrlEncode(fulluri);
    
    if (conf->dnlists != NULL) p = conf->dnlists;
    else p = getenv("GRST_DN_LISTS");
 
    if (p == NULL) p = GRST_DN_LISTS;
    dn_list_ptr = apr_pstrdup(r->pool, p);

    while ((dirname = strsep(&dn_list_ptr, ":")) != NULL)
       {
         filename = recurse4file(dirname, encfulluri, r->pool, 0);

         if (filename == NULL) continue;
    
         fd = open(filename, O_RDONLY);

         if (fd == -1) continue;

         fstat(fd, &statbuf);         
         ap_set_content_length(r, (apr_off_t) statbuf.st_size);
         r->mtime = apr_time_from_sec(statbuf.st_mtime);
         ap_set_content_type(r, "text/plain");
         ap_set_last_modified(r);

         buf = apr_palloc(r->pool, statbuf.st_size + 1);
         read(fd, buf, statbuf.st_size);
         buf[statbuf.st_size] = '\0';
            
         ap_rputs(buf, r);

         close(fd);

         return OK;
       }

    return HTTP_NOT_FOUND;
}

static void *create_gridsite_srv_config(apr_pool_t *p, server_rec *s)
{
    mod_gridsite_srv_cfg *srv_conf = apr_palloc(p, sizeof(*srv_conf));

    srv_conf->onetimesdir = apr_pstrdup(p, "/var/www/onetimes");
                                     /* GridSiteOnetimesDir dir-path */
    return srv_conf;
}

static void *merge_gridsite_srv_config(apr_pool_t *p, void *srvroot,
                                                      void *vhost)
/* merge virtual host with server-wide configs */
{
    mod_gridsite_srv_cfg *conf;;

    conf = apr_palloc(p, sizeof(*conf));

    conf->onetimesdir = ((mod_gridsite_srv_cfg *) srvroot)->onetimesdir;
                
    return conf;
}

static void *create_gridsite_dir_config(apr_pool_t *p, char *path)
{
    mod_gridsite_dir_cfg *conf = apr_palloc(p, sizeof(*conf));

    if (path == NULL) /* set up document root defaults */
      {
        conf->auth          = 0;     /* GridSiteAuth          on/off       */
        conf->envs          = 1;     /* GridSiteEnvs          on/off       */
        conf->format        = 0;     /* GridSiteHtmlFormat    on/off       */
        conf->indexes       = 0;     /* GridSiteIndexes       on/off       */
        conf->indexheader   = NULL;  /* GridSiteIndexHeader   File-value   */
        conf->gridsitelink  = 1;     /* GridSiteLink          on/off       */
        conf->adminfile     = apr_pstrdup(p, GRST_ADMIN_FILE);
                                /* GridSiteAdminFile      File-value   */
        conf->adminuri      = NULL;  /* GridSiteAdminURI      URI-value    */
        conf->helpuri       = NULL;  /* GridSiteHelpURI       URI-value    */
        conf->dnlists       = NULL;  /* GridSiteDNlists       Search-path  */
        conf->dnlistsuri    = NULL;  /* GridSiteDNlistsURI    URI-value    */
        conf->adminlist     = NULL;  /* GridSiteAdminList     URI-value    */
        conf->gsiproxylimit = 1;     /* GridSiteGSIProxyLimit number       */
        conf->unzip         = NULL;  /* GridSiteUnzip         file-path    */

        conf->methods    = apr_pstrdup(p, " GET ");
                                        /* GridSiteMethods      methods   */

        conf->editable = apr_pstrdup(p, " txt shtml html htm css js php jsp ");
                                        /* GridSiteEditable     types   */

        conf->headfile = apr_pstrdup(p, GRST_HEADFILE);
        conf->footfile = apr_pstrdup(p, GRST_FOOTFILE);
               /* GridSiteHeadFile and GridSiteFootFile  file name */

        conf->gridhttp       = 0;     /* GridSiteGridHTTP     on/off       */
        conf->soap2cgi      = 0;     /* GridSiteSoap2cgi      on/off       */
	conf->aclformat     = apr_pstrdup(p, "GACL");
                                     /* GridSiteACLFormat     gacl/xacml */
	conf->execmethod    = NULL;
               /* GridSiteExecMethod  nosetuid/suexec/X509DN/directory */
               
        conf->execugid.uid     = 0;	/* GridSiteUserGroup User Group */
        conf->execugid.gid     = 0;	/* ditto */
        conf->execugid.userdir = 0;	/* ditto */
        
        conf->diskmode	= APR_UREAD | APR_UWRITE; 
              /* GridSiteDiskMode group-mode world-mode
                 GroupNone | GroupRead | GroupWrite   WorldNone | WorldRead */
      }
    else
      {
        conf->auth          = UNSET; /* GridSiteAuth          on/off       */
        conf->envs          = UNSET; /* GridSiteEnvs          on/off       */
        conf->format        = UNSET; /* GridSiteHtmlFormat    on/off       */
        conf->indexes       = UNSET; /* GridSiteIndexes       on/off       */
        conf->indexheader   = NULL;  /* GridSiteIndexHeader   File-value   */
        conf->gridsitelink  = UNSET; /* GridSiteLink          on/off       */
        conf->adminfile     = NULL;  /* GridSiteAdminFile     File-value   */
        conf->adminuri      = NULL;  /* GridSiteAdminURI      URI-value    */
        conf->helpuri       = NULL;  /* GridSiteHelpURI       URI-value    */
        conf->dnlists       = NULL;  /* GridSiteDNlists       Search-path  */
        conf->dnlistsuri    = NULL;  /* GridSiteDNlistsURI    URI-value    */
        conf->adminlist     = NULL;  /* GridSiteAdminList     URI-value    */
        conf->gsiproxylimit = UNSET; /* GridSiteGSIProxyLimit number       */
        conf->unzip         = NULL;  /* GridSiteUnzip         file-path    */
        conf->methods       = NULL;  /* GridSiteMethods       methods      */
        conf->editable      = NULL;  /* GridSiteEditable      types        */
        conf->headfile      = NULL;  /* GridSiteHeadFile      file name    */
        conf->footfile      = NULL;  /* GridSiteFootFile      file name    */
        conf->gridhttp      = UNSET; /* GridSiteGridHTTP      on/off       */
        conf->soap2cgi      = UNSET; /* GridSiteSoap2cgi      on/off       */
	conf->aclformat     = NULL;  /* GridSiteACLFormat     gacl/xacml   */
	conf->execmethod    = NULL;  /* GridSiteExecMethod */
        conf->execugid.uid     = UNSET;	/* GridSiteUserGroup User Group */
        conf->execugid.gid     = UNSET; /* ditto */
        conf->execugid.userdir = UNSET; /* ditto */
        conf->diskmode	    = UNSET; /* GridSiteDiskMode group world */
      }

    return conf;
}

static void *merge_gridsite_dir_config(apr_pool_t *p, void *vserver,
                                                      void *vdirect)
/* merge directory with server-wide directory configs */
{
    mod_gridsite_dir_cfg *conf, *server, *direct;

    server = (mod_gridsite_dir_cfg *) vserver;
    direct = (mod_gridsite_dir_cfg *) vdirect;
    conf = apr_palloc(p, sizeof(*conf));

    if (direct->auth != UNSET) conf->auth = direct->auth;
    else                       conf->auth = server->auth;

    if (direct->envs != UNSET) conf->envs = direct->envs;
    else                       conf->envs = server->envs;
        
    if (direct->format != UNSET) conf->format = direct->format;
    else                         conf->format = server->format;
        
    if (direct->indexes != UNSET) conf->indexes = direct->indexes;
    else                          conf->indexes = server->indexes;
        
    if (direct->gridsitelink != UNSET) conf->gridsitelink=direct->gridsitelink;
    else                               conf->gridsitelink=server->gridsitelink;

    if (direct->indexheader != NULL) conf->indexheader = direct->indexheader;
    else                             conf->indexheader = server->indexheader;
        
    if (direct->adminfile != NULL) conf->adminfile = direct->adminfile;
    else                           conf->adminfile = server->adminfile;
        
    if (direct->adminuri != NULL) conf->adminuri = direct->adminuri;
    else                          conf->adminuri = server->adminuri;
        
    if (direct->helpuri != NULL) conf->helpuri = direct->helpuri;
    else                         conf->helpuri = server->helpuri;
        
    if (direct->dnlists != NULL) conf->dnlists = direct->dnlists;
    else                         conf->dnlists = server->dnlists;
        
    if (direct->dnlistsuri != NULL) conf->dnlistsuri = direct->dnlistsuri;
    else                            conf->dnlistsuri = server->dnlistsuri;

    if (direct->adminlist != NULL) conf->adminlist = direct->adminlist;
    else                           conf->adminlist = server->adminlist;

    if (direct->gsiproxylimit != UNSET)
                         conf->gsiproxylimit = direct->gsiproxylimit;
    else                 conf->gsiproxylimit = server->gsiproxylimit;

    if (direct->unzip != NULL) conf->unzip = direct->unzip;
    else                       conf->unzip = server->unzip;

    if (direct->methods != NULL) conf->methods = direct->methods;
    else                         conf->methods = server->methods;

    if (direct->editable != NULL) conf->editable = direct->editable;
    else                          conf->editable = server->editable;

    if (direct->headfile != NULL) conf->headfile = direct->headfile;
    else                          conf->headfile = server->headfile;

    if (direct->footfile != NULL) conf->footfile = direct->footfile;
    else                          conf->footfile = server->footfile;

    if (direct->gridhttp != UNSET) conf->gridhttp = direct->gridhttp;
    else                           conf->gridhttp = server->gridhttp;
        
    if (direct->soap2cgi != UNSET) conf->soap2cgi = direct->soap2cgi;
    else                           conf->soap2cgi = server->soap2cgi;

    if (direct->aclformat != NULL) conf->aclformat = direct->aclformat;
    else                           conf->aclformat = server->aclformat;

    if (direct->execmethod != NULL) conf->execmethod = direct->execmethod;
    else                            conf->execmethod = server->execmethod;

    if (direct->execugid.uid != UNSET)
      { conf->execugid.uid = direct->execugid.uid;
        conf->execugid.gid = direct->execugid.gid;
        conf->execugid.userdir = direct->execugid.userdir; }
    else
      { conf->execugid.uid = server->execugid.uid;
        conf->execugid.gid = server->execugid.gid;
        conf->execugid.userdir = server->execugid.userdir; }

    if (direct->diskmode != UNSET) conf->diskmode = direct->diskmode;
    else                            conf->diskmode = server->diskmode;
        
    return conf;
}

static const char *mod_gridsite_take1_cmds(cmd_parms *a, void *cfg,
                                           const char *parm)
{
    int   n;
    char *p;
    mod_gridsite_srv_cfg *srv_cfg = 
     (mod_gridsite_srv_cfg *) ap_get_module_config(a->server->module_config, 
     &gridsite_module);
  
    if (strcasecmp(a->cmd->name, "GridSiteOnetimesDir") == 0)
    {
      if (a->server->is_virtual)
       return "GridSiteOnetimesDir cannot be used inside a virtual server";
    
      srv_cfg->onetimesdir = apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteAdminFile") == 0)
    {
      if (index(parm, '/') != NULL) 
           return "/ not permitted in GridSiteAdminFile";
     
      ((mod_gridsite_dir_cfg *) cfg)->adminfile =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteAdminURI") == 0)
    {
      if (*parm != '/') return "GridSiteAdminURI must begin with /";
     
      ((mod_gridsite_dir_cfg *) cfg)->adminuri =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteHelpURI") == 0)
    {
      if (*parm != '/') return "GridSiteHelpURI must begin with /";

      ((mod_gridsite_dir_cfg *) cfg)->helpuri =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteDNlists") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->dnlists =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteDNlistsURI") == 0)
    {
      if (*parm != '/') return "GridSiteDNlistsURI must begin with /";

      if ((*parm != '\0') && (parm[strlen(parm) - 1] == '/'))
       ((mod_gridsite_dir_cfg *) cfg)->dnlistsuri =
        apr_pstrdup(a->pool, parm);
      else
       ((mod_gridsite_dir_cfg *) cfg)->dnlistsuri =
        apr_pstrcat(a->pool, parm, "/", NULL);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteAdminList") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->adminlist =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteGSIProxyLimit") == 0)
    {
      n = -1;
    
      if ((sscanf(parm, "%d", &n) == 1) && (n >= 0))
                  ((mod_gridsite_dir_cfg *) cfg)->gsiproxylimit = n;
      else return "GridSiteGSIProxyLimit must be a number >= 0";     
    }
    else if (strcasecmp(a->cmd->name, "GridSiteUnzip") == 0)
    {
      if (*parm != '/') return "GridSiteUnzip must begin with /";
     
      ((mod_gridsite_dir_cfg *) cfg)->unzip =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteMethods") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->methods =
        apr_psprintf(a->pool, " %s ", parm);
       
      for (p = ((mod_gridsite_dir_cfg *) cfg)->methods;
           *p != '\0';
           ++p) if (*p == '\t') *p = ' ';
    }
    else if (strcasecmp(a->cmd->name, "GridSiteEditable") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->editable =
        apr_psprintf(a->pool, " %s ", parm);
     
      for (p = ((mod_gridsite_dir_cfg *) cfg)->editable;
           *p != '\0';
           ++p) if (*p == '\t') *p = ' ';
    }
    else if (strcasecmp(a->cmd->name, "GridSiteHeadFile") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->headfile =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteFootFile") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->footfile =
        apr_pstrdup(a->pool, parm);
    }  
    else if (strcasecmp(a->cmd->name, "GridSiteIndexHeader") == 0)
    {
      if (index(parm, '/') != NULL) 
           return "/ not permitted in GridSiteIndexHeader";

      ((mod_gridsite_dir_cfg *) cfg)->indexheader =
        apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteACLFormat") == 0)
    {
      if ((strcasecmp(parm,"GACL") != 0) &&
          (strcasecmp(parm,"XACML") != 0))
          return "GridsiteACLFormat must be either GACL or XACML";
      
      ((mod_gridsite_dir_cfg *) cfg)->aclformat = apr_pstrdup(a->pool, parm);
    }
    else if (strcasecmp(a->cmd->name, "GridSiteExecMethod") == 0)
    {
      if (strcasecmp(parm, "nosetuid") == 0)
        {
          ((mod_gridsite_dir_cfg *) cfg)->execmethod = NULL;
          return NULL;
        }
    
      if ((strcasecmp(parm, "suexec")    != 0) &&
          (strcasecmp(parm, "X509DN")    != 0) &&
          (strcasecmp(parm, "directory") != 0))
          return "GridsiteExecMethod must be nosetuid, suexec, X509DN or directory";
      
      ((mod_gridsite_dir_cfg *) cfg)->execmethod = apr_pstrdup(a->pool, parm);
    }

    return NULL;
}

static const char *mod_gridsite_take2_cmds(cmd_parms *a, void *cfg,
                                       const char *parm1, const char *parm2)
{
    if (strcasecmp(a->cmd->name, "GridSiteUserGroup") == 0)
    {
      if (!(unixd_config.suexec_enabled))
          return "Using GridSiteUserGroup will "
                 "require rebuilding Apache with suexec support!";
    
      /* NB ap_uname2id/ap_gname2id are NOT thread safe - but OK
         as long as not used in .htaccess, just at server start time */

      ((mod_gridsite_dir_cfg *) cfg)->execugid.uid = ap_uname2id(parm1);
      ((mod_gridsite_dir_cfg *) cfg)->execugid.gid = ap_gname2id(parm2);
      ((mod_gridsite_dir_cfg *) cfg)->execugid.userdir = 0;
    }
    else if (strcasecmp(a->cmd->name, "GridSiteDiskMode") == 0)
    {
      if ((strcasecmp(parm1, "GroupNone" ) != 0) &&
          (strcasecmp(parm1, "GroupRead" ) != 0) &&
          (strcasecmp(parm1, "GroupWrite") != 0))
        return "First parameter of GridSiteDiskMode must be "
               "GroupNone, GroupRead or GroupWrite!";
          
      if ((strcasecmp(parm2, "WorldNone" ) != 0) &&
          (strcasecmp(parm2, "WorldRead" ) != 0))
        return "Second parameter of GridSiteDiskMode must be "
               "WorldNone or WorldRead!";
          
      ((mod_gridsite_dir_cfg *) cfg)->diskmode = 
       APR_UREAD | APR_UWRITE 
       | ( APR_GREAD               * (strcasecmp(parm1, "GroupRead") == 0))
       | ((APR_GREAD | APR_GWRITE) * (strcasecmp(parm1, "GroupWrite") == 0))
       | ((APR_GREAD | APR_WREAD)  * (strcasecmp(parm2, "WorldRead") == 0));
    }
    
    return NULL;
}

static const char *mod_gridsite_flag_cmds(cmd_parms *a, void *cfg,
                                      int flag)
{
    if      (strcasecmp(a->cmd->name, "GridSiteAuth") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->auth = flag;
    }
    else if (strcasecmp(a->cmd->name, "GridSiteEnvs") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->envs = flag;
    }
    else if (strcasecmp(a->cmd->name, "GridSiteHtmlFormat") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->format = flag;
    }
    else if (strcasecmp(a->cmd->name, "GridSiteIndexes") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->indexes = flag;
    }
    else if (strcasecmp(a->cmd->name, "GridSiteLink") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->gridsitelink = flag;
    }
    else if (strcasecmp(a->cmd->name, "GridSiteGridHTTP") == 0)
    {
// TODO: return error if try this on non-HTTPS virtual server

      ((mod_gridsite_dir_cfg *) cfg)->gridhttp = flag;
    }
    else if (strcasecmp(a->cmd->name, "GridSiteSoap2cgi") == 0)
    {
      ((mod_gridsite_dir_cfg *) cfg)->soap2cgi = flag;
    }

    return NULL;
}

static const command_rec mod_gridsite_cmds[] =
{
// TODO: need to check and document valid contexts for each command!

    AP_INIT_FLAG("GridSiteAuth", mod_gridsite_flag_cmds, 
                 NULL, OR_FILEINFO, "on or off"),
    AP_INIT_FLAG("GridSiteEnvs", mod_gridsite_flag_cmds, 
                 NULL, OR_FILEINFO, "on or off"),
    AP_INIT_FLAG("GridSiteHtmlFormat", mod_gridsite_flag_cmds, 
                 NULL, OR_FILEINFO, "on or off"),
    AP_INIT_FLAG("GridSiteIndexes", mod_gridsite_flag_cmds, 
                 NULL, OR_FILEINFO, "on or off"),
    AP_INIT_FLAG("GridSiteLink", mod_gridsite_flag_cmds, 
                 NULL, OR_FILEINFO, "on or off"),
                 
    AP_INIT_TAKE1("GridSiteAdminFile", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "Ghost per-directory admin CGI"),
    AP_INIT_TAKE1("GridSiteAdminURI", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "URI of real gridsite-admin.cgi"),
    AP_INIT_TAKE1("GridSiteHelpURI", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "URI of Website Help pages"),
    AP_INIT_TAKE1("GridSiteDNlists", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "DN Lists directories search path"),
    AP_INIT_TAKE1("GridSiteDNlistsURI", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "URI of published DN lists"),
    AP_INIT_TAKE1("GridSiteAdminList", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "URI of admin DN List"),
    AP_INIT_TAKE1("GridSiteGSIProxyLimit", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "Max level of GSI proxy validity"),
    AP_INIT_TAKE1("GridSiteUnzip", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "Absolute path to unzip command"),

    AP_INIT_RAW_ARGS("GridSiteMethods", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "permitted HTTP methods"),
    AP_INIT_RAW_ARGS("GridSiteEditable", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "editable file extensions"),
    AP_INIT_TAKE1("GridSiteHeadFile", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "filename of HTML header"),
    AP_INIT_TAKE1("GridSiteFootFile", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "filename of HTML footer"),
    AP_INIT_TAKE1("GridSiteIndexHeader", mod_gridsite_take1_cmds,
                   NULL, OR_FILEINFO, "filename of directory header"),
    
    AP_INIT_FLAG("GridSiteGridHTTP", mod_gridsite_flag_cmds,
                 NULL, OR_FILEINFO, "on or off"),
    AP_INIT_TAKE1("GridSiteOnetimesDir", mod_gridsite_take1_cmds,
                 NULL, RSRC_CONF, "directory with GridHTTP onetime passcodes"),
    
    AP_INIT_FLAG("GridSiteSoap2cgi", mod_gridsite_flag_cmds,
                 NULL, OR_FILEINFO, "on or off"),

    AP_INIT_TAKE1("GridSiteACLFormat", mod_gridsite_take1_cmds,
                 NULL, OR_FILEINFO, "format to save access control lists in"),

    AP_INIT_TAKE1("GridSiteExecMethod", mod_gridsite_take1_cmds,
                 NULL, OR_FILEINFO, "execution strategy used by gsexec"),
                 
    AP_INIT_TAKE2("GridSiteUserGroup", mod_gridsite_take2_cmds, 
                  NULL, OR_FILEINFO,
                  "user and group of gsexec processes in suexec mode"),
          
    AP_INIT_TAKE2("GridSiteDiskMode", mod_gridsite_take2_cmds, 
                  NULL, OR_FILEINFO,
                  "group and world file modes for new files/directories"),
          
    {NULL}
};

static int mod_gridsite_first_fixups(request_rec *r)
{
    mod_gridsite_dir_cfg *conf;

    if (r->finfo.filetype != APR_DIR) return DECLINED;

    conf = (mod_gridsite_dir_cfg *)
                    ap_get_module_config(r->per_dir_config, &gridsite_module);

    /* we handle DN Lists as regular files, even if they also match
       directory names  */

    if ((conf != NULL) &&
        (conf->dnlistsuri != NULL) &&
        (strncmp(r->uri, conf->dnlistsuri, strlen(conf->dnlistsuri)) == 0) &&
        (strcmp(r->uri, conf->dnlistsuri) != 0))
      {
        r->finfo.filetype = APR_REG; 
      }

    return DECLINED;
}  

static int mod_gridsite_perm_handler(request_rec *r)
/*
    Do authentication/authorization here rather than in the normal module
    auth functions since the results of mod_ssl are available.

    We also publish environment variables here if requested by GridSiteEnv.
*/
{
    int          retcode = DECLINED, i, n, file_is_acl = 0,
                 destination_is_acl = 0;
    char        *dn, *p, envname[14], *grst_cred_0 = NULL, *dir_path, 
                *remotehost, s[99], *grst_cred_i, *cookies, *file,
                *gridauthonetime, *cookiefile, oneline[1025], *key_i,
                *destination = NULL, *destination_uri = NULL, 
                *destination_prefix = NULL, *destination_translated = NULL;
    const char  *content_type;
    time_t       now, notbefore, notafter;
    apr_table_t *env;
    apr_finfo_t  cookiefile_info;
    apr_file_t  *fp;
    request_rec *destreq;
    GRSTgaclCred    *cred = NULL, *cred_0 = NULL;
    GRSTgaclUser    *user = NULL;
    GRSTgaclPerm     perm = GRST_PERM_NONE, destination_perm = GRST_PERM_NONE;
    GRSTgaclAcl     *acl = NULL;
    mod_gridsite_dir_cfg *cfg;

    cfg = (mod_gridsite_dir_cfg *)
                    ap_get_module_config(r->per_dir_config, &gridsite_module);

    if (cfg == NULL) return DECLINED;

    if ((cfg->auth == 0) &&
        (cfg->envs == 0))
               return DECLINED; /* if not turned on, look invisible */

    env = r->subprocess_env;

    /* do we need/have per-connection (SSL) cred variable(s)? */

    if ((user == NULL) && 
        (r->connection->notes != NULL) &&
        ((grst_cred_0 = (char *) 
            apr_table_get(r->connection->notes, "GRST_CRED_0")) != NULL))
      {
        if (((mod_gridsite_dir_cfg *) cfg)->envs)
                            apr_table_setn(env, "GRST_CRED_0", grst_cred_0);
                                    
        cred_0 = GRSTx509CompactToCred(grst_cred_0);
        if ((cred_0 != NULL) &&
            (GRSTgaclCredGetDelegation(cred_0) 
                         <= ((mod_gridsite_dir_cfg *) cfg)->gsiproxylimit))
          {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "Using identity %s from SSL/TLS", grst_cred_0);

            user = GRSTgaclUserNew(cred_0);

            /* check for VOMS GRST_CRED_i too */
  
            for (i=1; ; ++i)
               {
                 snprintf(envname, sizeof(envname), "GRST_CRED_%d", i);
                 if (grst_cred_i = (char *) 
                                   apr_table_get(r->connection->notes,envname))
                   { 
                     if (((mod_gridsite_dir_cfg *) cfg)->envs)
                              apr_table_setn(env,
                                             apr_pstrdup(r->pool, envname),
                                             grst_cred_i);
                                    
                     if (cred = GRSTx509CompactToCred(grst_cred_i))
                                        GRSTgaclUserAddCred(user, cred);
                   }
                 else break; /* GRST_CRED_i are numbered consecutively */
               }
          }
      }

    if ((user != NULL) && ((mod_gridsite_dir_cfg *) cfg)->dnlists)
          GRSTgaclUserSetDNlists(user, ((mod_gridsite_dir_cfg *) cfg)->dnlists);

    /* add DNS credential */
    
    remotehost = (char *) ap_get_remote_host(r->connection,
                                  r->per_dir_config, REMOTE_DOUBLE_REV, NULL);
    if ((remotehost != NULL) && (*remotehost != '\0'))
      {            
        cred = GRSTgaclCredNew("dns");
        GRSTgaclCredAddValue(cred, "hostname", remotehost);

        if (user == NULL) user = GRSTgaclUserNew(cred);
        else              GRSTgaclUserAddCred(user, cred);
      }

    /* check for Destination: header and evaluate if present */

    if ((destination = (char *) apr_table_get(r->headers_in,
                                              "Destination")) != NULL)
      {
        destination_prefix = apr_psprintf(r->pool, "https://%s:%d/", 
                         r->server->server_hostname, (int) r->server->port);

        if (strncmp(destination_prefix, destination,
                    strlen(destination_prefix)) == 0) 
           destination_uri = &destination[strlen(destination_prefix)-1];
        else if ((int) r->server->port == 443)
          {
            destination_prefix = apr_psprintf(r->pool, "https://%s/", 
                                              r->server->server_hostname);

            if (strncmp(destination_prefix, destination,
                                strlen(destination_prefix)) == 0)
              destination_uri = &destination[strlen(destination_prefix)-1];
          }
          
        if (destination_uri != NULL)
          {
            destreq = ap_sub_req_method_uri("GET", destination_uri, r, NULL);

            if ((destreq != NULL) && (destreq->filename != NULL) 
                                  && (destreq->path_info != NULL))
              {
                destination_translated = apr_pstrcat(r->pool, 
                               destreq->filename, destreq->path_info, NULL);

                apr_table_setn(r->notes, "GRST_DESTINATION_TRANSLATED", 
                               destination_translated);
                             
                if (((mod_gridsite_dir_cfg *) cfg)->envs)
                        apr_table_setn(env, "GRST_DESTINATION_TRANSLATED", 
                                                  destination_translated);
                                                  
                 p = rindex(destination_translated, '/');
                 if ((p != NULL) && (strcmp(&p[1], GRST_ACL_FILE) == 0))
                                                    destination_is_acl = 1;
              }
          }
      }
    
    /* this checks for NULL arguments itself */
    if (GRSTgaclDNlistHasUser(((mod_gridsite_dir_cfg *) cfg)->adminlist, user))
      {
        perm = GRST_PERM_ALL;
        if (destination_translated != NULL) destination_perm = GRST_PERM_ALL;
      }
    else
      {
        acl = GRSTgaclAclLoadforFile(r->filename);
        if (acl != NULL) perm = GRSTgaclAclTestUser(acl, user);
        GRSTgaclAclFree(acl);
        
        if (destination_translated != NULL)
          {
            acl = GRSTgaclAclLoadforFile(destination_translated);
            if (acl != NULL) destination_perm = GRSTgaclAclTestUser(acl, user);
            GRSTgaclAclFree(acl);

            apr_table_setn(r->notes, "GRST_DESTINATION_PERM",
                              apr_psprintf(r->pool, "%d", destination_perm));
          
            if (((mod_gridsite_dir_cfg *) cfg)->envs)
              apr_table_setn(env, "GRST_DESTINATION_PERM",
                              apr_psprintf(r->pool, "%d", destination_perm));
          }
      }
      
    if ((p = (char *) apr_table_get(r->headers_in, "Cookie")) != NULL)
      {
        cookies = apr_pstrcat(r->pool, " ", p, NULL);
        gridauthonetime = strstr(cookies, " GRIDHTTP_ONETIME=");
                
        if (gridauthonetime != NULL)
          {
            for (p = &gridauthonetime[18]; (*p != '\0') && (*p != ';'); ++p)
                                                if (!isalnum(*p)) *p = '_';
        
            cookiefile = apr_psprintf(r->pool, "%s/%s",
                 ap_server_root_relative(r->pool,
                   ((mod_gridsite_srv_cfg *) 
                    ap_get_module_config(r->server->module_config, 
                                    &gridsite_module))->onetimesdir),
                 &gridauthonetime[18]);
                                      
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "Opening GridHTTP onetime file %s", cookiefile);
              
            if ((apr_stat(&cookiefile_info, cookiefile, 
                          APR_FINFO_TYPE, r->pool) == APR_SUCCESS) &&
                (cookiefile_info.filetype == APR_REG) &&
                (apr_file_open(&fp, cookiefile, APR_READ, 0, r->pool)
                                                         == APR_SUCCESS))
              {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                             "Reading GridHTTP onetime file %s", cookiefile);
              
                while (apr_file_gets(oneline, 
                                     sizeof(oneline), fp) == APR_SUCCESS)
                     {
                       p = index(oneline, '\n');
                       if (p != NULL) *p = '\0';
                       
                       ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                                    "%s: %s", cookiefile, oneline);

                       if ((strncmp(oneline, "expires=", 8) == 0) &&
                           (apr_time_from_sec(atoll(&oneline[8])) < 
                                                       apr_time_now()))
                                  break;                
                       else if ((strncmp(oneline, "domain=", 7) == 0) &&
                                (strcmp(&oneline[7], r->hostname) != 0))
                                  break; /* exact needed in the version */
                       else if ((strncmp(oneline, "path=", 5) == 0) &&
                                (strcmp(&oneline[5], r->uri) != 0))
                                  break;
                       else if  (strncmp(oneline, "onetime=yes", 11) == 0)
                                  apr_file_remove(cookiefile, r->pool);                                  
                       else if  (strncmp(oneline, "method=PUT", 10) == 0)
                                  perm |= GRST_PERM_WRITE;
                       else if  (strncmp(oneline, "method=GET", 10) == 0)
                                  perm |= GRST_PERM_READ;
                     }

                apr_file_close(fp);
              }            
          }
      }
    
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                 "After GACL/Onetime evaluation, GRST_PERM=%d", perm);

    /* set permission and GACL environment variables */
    
    apr_table_setn(r->notes, "GRST_PERM", apr_psprintf(r->pool, "%d", perm));

    if (((mod_gridsite_dir_cfg *) cfg)->envs)
      {
        apr_table_setn(env, "GRST_PERM", apr_psprintf(r->pool, "%d", perm));

        if (((dir_path = apr_pstrdup(r->pool, r->filename)) != NULL) &&
            ((p = rindex(dir_path, '/')) != NULL))
          {
            *p = '\0';
            apr_table_setn(env, "GRST_DIR_PATH", dir_path);
          }

        if (((mod_gridsite_dir_cfg *) cfg)->helpuri != NULL)
                  apr_table_setn(env, "GRST_HELP_URI",
                              ((mod_gridsite_dir_cfg *) cfg)->helpuri);

        if (((mod_gridsite_dir_cfg *) cfg)->adminfile != NULL)
                  apr_table_setn(env, "GRST_ADMIN_FILE",
                              ((mod_gridsite_dir_cfg *) cfg)->adminfile);

        if (((mod_gridsite_dir_cfg *) cfg)->editable != NULL)
	          apr_table_setn(env, "GRST_EDITABLE",
                              ((mod_gridsite_dir_cfg *) cfg)->editable);

        if (((mod_gridsite_dir_cfg *) cfg)->headfile != NULL)
	          apr_table_setn(env, "GRST_HEAD_FILE",
                              ((mod_gridsite_dir_cfg *) cfg)->headfile);

        if (((mod_gridsite_dir_cfg *) cfg)->footfile != NULL)
	          apr_table_setn(env, "GRST_FOOT_FILE",
                              ((mod_gridsite_dir_cfg *) cfg)->footfile);

        if (((mod_gridsite_dir_cfg *) cfg)->dnlists != NULL)
	          apr_table_setn(env, "GRST_DN_LISTS",
                              ((mod_gridsite_dir_cfg *) cfg)->dnlists);

        if (((mod_gridsite_dir_cfg *) cfg)->dnlistsuri != NULL)
	          apr_table_setn(env, "GRST_DN_LISTS_URI",
                              ((mod_gridsite_dir_cfg *) cfg)->dnlistsuri);

        if (((mod_gridsite_dir_cfg *) cfg)->adminlist != NULL)
	          apr_table_setn(env, "GRST_ADMIN_LIST",
                              ((mod_gridsite_dir_cfg *) cfg)->adminlist);

	apr_table_setn(env, "GRST_GSIPROXY_LIMIT",
 	                     apr_psprintf(r->pool, "%d",
  	                           ((mod_gridsite_dir_cfg *)cfg)->gsiproxylimit));

        if (((mod_gridsite_dir_cfg *) cfg)->unzip != NULL)
	          apr_table_setn(env, "GRST_UNZIP",
                              ((mod_gridsite_dir_cfg *) cfg)->unzip);

        if (!(((mod_gridsite_dir_cfg *) cfg)->gridsitelink))
                  apr_table_setn(env, "GRST_NO_LINK", "1");

        if (((mod_gridsite_dir_cfg *) cfg)->aclformat != NULL)
	          apr_table_setn(env, "GRST_ACL_FORMAT",
                              ((mod_gridsite_dir_cfg *) cfg)->aclformat);

        if (((mod_gridsite_dir_cfg *) cfg)->execmethod != NULL)
          {
	    apr_table_setn(env, "GRST_EXEC_METHOD",
                              ((mod_gridsite_dir_cfg *) cfg)->execmethod);
                              
            if ((strcasecmp(((mod_gridsite_dir_cfg *) cfg)->execmethod,  
                           "directory") == 0) && (r->filename != NULL))
              {
                if ((r->content_type != NULL) && 
                    (strcmp(r->content_type, DIR_MAGIC_TYPE) == 0))
                  apr_table_setn(env, "GRST_EXEC_DIRECTORY", r->filename);
                else
                  {
                    file = apr_pstrdup(r->pool, r->filename);
                    p = rindex(file, '/');
                    if (p != NULL)
                      {
                        *p = '\0';
                        apr_table_setn(env, "GRST_EXEC_DIRECTORY", file);
                      }                    
                  }                 
              }
          }

        apr_table_setn(env, "GRST_DISK_MODE",
 	                     apr_psprintf(r->pool, "0x%04x",
    	                      ((mod_gridsite_dir_cfg *)cfg)->diskmode));
      }

    if (((mod_gridsite_dir_cfg *) cfg)->auth)
      {
        /* *** Check HTTP method to decide which perm bits to check *** */

        if ((r->filename != NULL) && 
            ((p = rindex(r->filename, '/')) != NULL) &&
            (strcmp(&p[1], GRST_ACL_FILE) == 0)) file_is_acl = 1;

        content_type = r->content_type;
        if ((content_type != NULL) &&
            (strcmp(content_type, DIR_MAGIC_TYPE) == 0) &&
            (((mod_gridsite_dir_cfg *) cfg)->dnlistsuri != NULL) &&
            (strncmp(r->uri,
                     ((mod_gridsite_dir_cfg *) cfg)->dnlistsuri,
                     strlen(((mod_gridsite_dir_cfg *) cfg)->dnlistsuri)) == 0) &&
            (strlen(r->uri) > strlen(((mod_gridsite_dir_cfg *) cfg)->dnlistsuri)))
            content_type = "text/html";

        if ( GRSTgaclPermHasNone(perm) ||

            /* first two M_GET conditions make the subtle distinction
               between .../ that maps to .../index.html (governed by
               Read perm) or to dir list (governed by List perm);
               third M_GET condition deals with typeless CGI requests */

            ((r->method_number == M_GET) &&
             !GRSTgaclPermHasRead(perm)  &&
             (content_type != NULL)   &&
             (strcmp(content_type, DIR_MAGIC_TYPE) != 0)) ||

            ((r->method_number == M_GET) &&
             !GRSTgaclPermHasList(perm)  &&
             (content_type != NULL)   &&
             (strcmp(content_type, DIR_MAGIC_TYPE) == 0)) ||

            ((r->method_number == M_GET) &&
             !GRSTgaclPermHasRead(perm)  &&
             (content_type == NULL))      ||

            ((r->method_number == M_POST) && !GRSTgaclPermHasRead(perm) ) ||

            (((r->method_number == M_PUT) || 
              (r->method_number == M_DELETE)) &&
             !GRSTgaclPermHasWrite(perm) && !file_is_acl) ||

            ((r->method_number == M_MOVE) &&
             ((!GRSTgaclPermHasWrite(perm) && !file_is_acl) || 
              (!GRSTgaclPermHasAdmin(perm) && file_is_acl)  ||
              (!GRSTgaclPermHasWrite(destination_perm) 
                                    && !destination_is_acl) || 
              (!GRSTgaclPermHasAdmin(destination_perm) 
                                     && destination_is_acl)) ) ||

            (((r->method_number == M_PUT) || 
              (r->method_number == M_DELETE)) &&
             !GRSTgaclPermHasAdmin(perm) && file_is_acl) 
             
             ) retcode = HTTP_FORBIDDEN;
      }

    return retcode;
}

int GRST_X509_check_issued_wrapper(X509_STORE_CTX *ctx,X509 *x,X509 *issuer)
/* We change the default callback to use our wrapper and discard errors
   due to GSI proxy chains (ie where users certs act as CAs) */
{
    int ret;
    ret = X509_check_issued(issuer, x);
    if (ret == X509_V_OK)
                return 1;
         
    /* Non self-signed certs without signing are ok if they passed
           the other checks inside X509_check_issued. Is this enough? */
    if ((ret == X509_V_ERR_KEYUSAGE_NO_CERTSIGN) &&
        (X509_NAME_cmp(X509_get_subject_name(issuer),
                           X509_get_subject_name(x)) != 0)) return 1;
 
    /* If we haven't asked for issuer errors don't set ctx */
    if (!(ctx->flags & X509_V_FLAG_CB_ISSUER_CHECK)) return 0;
  
    ctx->error = ret;
    ctx->current_cert = x;
    ctx->current_issuer = issuer;
    return ctx->verify_cb(0, ctx);
}

/* Later OpenSSL versions add a second pointer ... */
int GRST_verify_cert_wrapper(X509_STORE_CTX *ctx, void *p)

/* Earlier ones have a single argument ... */
// int GRST_verify_cert_wrapper(X509_STORE_CTX *ctx)

/* Before 0.9.7 we cannot change the check_issued callback directly in
   the X509_STORE, so we must insert it in another callback that gets
   called early enough */
{
   ctx->check_issued = GRST_X509_check_issued_wrapper;

   return X509_verify_cert(ctx);
}

int GRST_callback_SSLVerify_wrapper(int ok, X509_STORE_CTX *ctx)
{
   SSL *ssl            = (SSL *) X509_STORE_CTX_get_app_data(ctx);
   conn_rec *conn      = (conn_rec *) SSL_get_app_data(ssl);
   server_rec *s       = conn->base_server;
   SSLConnRec *sslconn = 
         (SSLConnRec *) ap_get_module_config(conn->conn_config, &ssl_module);
   int errnum          = X509_STORE_CTX_get_error(ctx);
   int errdepth        = X509_STORE_CTX_get_error_depth(ctx);
   int returned_ok;
   int first_non_ca;

   /*
    * GSI Proxy user-cert-as-CA handling:
    * we skip Invalid CA errors at this stage, since we will check this
    * again at errdepth=0 for the full chain using GRSTx509CheckChain
    */
   if (errnum == X509_V_ERR_INVALID_CA)
     {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                    "Skip Invalid CA error in case a GSI Proxy");

        sslconn->verify_error = NULL;
        ok = TRUE;
        errnum = X509_V_OK;
        X509_STORE_CTX_set_error(ctx, errnum);
     }

   /*
    * New style GSI Proxy handling, with critical ProxyCertInfo
    * extension: we use GRSTx509KnownCriticalExts() to check this
    */
#ifndef X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION
#define X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION 34
#endif
   if (errnum == X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION)
     {
       if (GRSTx509KnownCriticalExts(X509_STORE_CTX_get_current_cert(ctx))
                                                              == GRST_RET_OK)
         {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                     "GRSTx509KnownCriticalExts() accepts previously "
                     "Unhandled Critical Extension (GSI Proxy?)");

            sslconn->verify_error = NULL;
            ok = TRUE;
            errnum = X509_V_OK;
            X509_STORE_CTX_set_error(ctx, errnum);
         }
     }

   returned_ok = ssl_callback_SSLVerify(ok, ctx);

   /* in case ssl_callback_SSLVerify changed it */
   errnum = X509_STORE_CTX_get_error(ctx); 

   if ((errdepth == 0) && (errnum == X509_V_OK))
   /*
    * We've now got the last certificate - the identity being used for
    * this connection. At this point we check the whole chain for valid
    * CAs or, failing that, GSI-proxy validity using GRSTx509CheckChain.
    */
     {
        errnum = GRSTx509CheckChain(&first_non_ca, ctx);

        if (errnum != X509_V_OK)
          {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Invalid certificate chain reported by "
                     "GRSTx509CheckChain()");

            sslconn->verify_error = X509_verify_cert_error_string(errnum);
            ok = FALSE;
          }
        else 
          {
            int i, lastcred;
            STACK_OF(X509) *peer_certs;
            const int maxcreds = 99;
            const size_t credlen = 1024;
            char creds[maxcreds][credlen+1], envname[14];

            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, "Valid certificate"
                                   " chain reported by GRSTx509CheckChain()");

            /*
             * Always put result of GRSTx509CompactCreds() into environment
             */
            if (peer_certs = (STACK_OF(X509) *) X509_STORE_CTX_get_chain(ctx))
              {    
                if (GRSTx509CompactCreds(&lastcred, maxcreds, credlen,
                    (char *) creds, peer_certs, GRST_VOMS_DIR) == GRST_RET_OK)
                  {
                    for (i=0; i <= lastcred; ++i)
                       {
                         apr_table_setn(conn->notes,
                                 apr_psprintf(conn->pool, "GRST_CRED_%d", i),
                                 apr_pstrdup(conn->pool, creds[i]));

                         ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                                      "store GRST_CRED_%d=%s", i, creds[i]);
                       }
                  }
            /* free remaining dup'd certs? */
              }                                   
          }
     }

   return returned_ok;
}

static int mod_gridsite_server_post_config(apr_pool_t *pPool,
                  apr_pool_t *pLog, apr_pool_t *pTemp, server_rec *main_server)
{
   SSL_CTX         *ctx;
   SSLSrvConfigRec *sc;
   server_rec      *this_server;

   ap_add_version_component(pPool,
                            apr_psprintf(pPool, "mod_gridsite/%s", VERSION));

   for (this_server = main_server; 
        this_server != NULL; 
        this_server = this_server->next)
      {
        sc = ap_get_module_config(this_server->module_config, &ssl_module);

        if ((sc                  != NULL)  &&
            (sc->enabled)                  &&
            (sc->server          != NULL)  &&
            (sc->server->ssl_ctx != NULL))
          {
            ctx = sc->server->ssl_ctx;

            /* in 0.9.7 we could set the issuer-checking callback directly */
//          ctx->cert_store->check_issued = GRST_X509_check_issued_wrapper;
     
            /* but in case 0.9.6 we do it indirectly with another wrapper */
            SSL_CTX_set_cert_verify_callback(ctx, 
                                             GRST_verify_cert_wrapper,
                                             (void *) NULL);

            /* whatever version, we can set the SSLVerify wrapper properly */
            SSL_CTX_set_verify(ctx, ctx->verify_mode, 
                               GRST_callback_SSLVerify_wrapper);

            if (main_server->loglevel >= APLOG_DEBUG)
                 ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, main_server,
                      "Set mod_ssl verify callbacks to GridSite wrappers");
          }
      }
      
   return OK;
}
      
static void mod_gridsite_child_init(apr_pool_t *pPool, server_rec *pServer)
{
   GRSTgaclInit();
}

static int mod_gridsite_handler(request_rec *r)
{
   mod_gridsite_dir_cfg *conf;
    
   conf = (mod_gridsite_dir_cfg *)
                    ap_get_module_config(r->per_dir_config, &gridsite_module);

   if ((conf->dnlistsuri != NULL) &&
       (strncmp(r->uri, conf->dnlistsuri, strlen(conf->dnlistsuri)) == 0))
     {
       if (strcmp(r->uri, conf->dnlistsuri) == 0)
              return mod_gridsite_dnlistsuri_dir_handler(r, conf);

       return mod_gridsite_dnlistsuri_handler(r, conf);
    }

   if (strcmp(r->handler, DIR_MAGIC_TYPE) == 0)
                   return mod_gridsite_dir_handler(r, conf);
   
   return mod_gridsite_nondir_handler(r, conf);
}

static ap_unix_identity_t *mod_gridsite_get_suexec_id_doer(const request_rec *r)
{
   mod_gridsite_dir_cfg *conf;
    
   conf = (mod_gridsite_dir_cfg *)
                    ap_get_module_config(r->per_dir_config, &gridsite_module);

   if ((conf->execugid.uid != UNSET) && 
       (conf->execmethod != NULL)) 
     {
     
     /* also push GRST_EXEC_DIRECTORY into request environment here too */
     
       return &(conf->execugid);
     }
              
   return NULL;
}

static void register_hooks(apr_pool_t *p)
{
    /* set up the Soap2cgi input and output filters */

    ap_hook_insert_filter(mod_gridsite_soap2cgi_insert, NULL, NULL,
                          APR_HOOK_MIDDLE);

    ap_register_output_filter(Soap2cgiFilterName, mod_gridsite_soap2cgi_out,
                              NULL, AP_FTYPE_RESOURCE);

//    ap_register_input_filter(Soap2cgiFilterName, mod_gridsite_soap2cgi_in,
//                              NULL, AP_FTYPE_RESOURCE);

    /* config and handler stuff */

    ap_hook_post_config(mod_gridsite_server_post_config, NULL, NULL, 
                                                              APR_HOOK_LAST);
    ap_hook_child_init(mod_gridsite_child_init, NULL, NULL, APR_HOOK_MIDDLE);

    ap_hook_fixups(mod_gridsite_first_fixups,NULL,NULL,APR_HOOK_FIRST);
    
    ap_hook_fixups(mod_gridsite_perm_handler,NULL,NULL,APR_HOOK_REALLY_LAST);
    
    ap_hook_handler(mod_gridsite_handler, NULL, NULL, APR_HOOK_FIRST);    
    
    ap_hook_get_suexec_identity(mod_gridsite_get_suexec_id_doer,
                                NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA gridsite_module =
{
    STANDARD20_MODULE_STUFF,
    create_gridsite_dir_config, /* dir config creater */
    merge_gridsite_dir_config,  /* dir merger */
    create_gridsite_srv_config, /* create server config */
    merge_gridsite_srv_config,  /* merge server config */
    mod_gridsite_cmds,          /* command apr_table_t */
    register_hooks              /* register hooks */
};
