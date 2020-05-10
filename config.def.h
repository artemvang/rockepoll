
#define MAXFDS 128
#define KEEP_ALIVE_TIMEOUT 5 /* in seconds */


#define DEFAULT_CONF_PORT         7887
#define DEFAULT_CONF_KEEP_ALIVE   0
#define DEFAULT_CONF_QUIET        0
#define DEFAULT_CONF_CHROOT       0
#define DEFAULT_CONF_LISTEN_ADDR  "127.0.0.1"
#define DEFAULT_CONF_ROOT_DIR     "."


#define INDEX_PAGE          "index.html"
#define DEFAULT_MIMETYPE    "application/octet-stream"
#define HTTP_STATUS_FORMAT  "<h1>%s</h1>"  // <h1>Not Found</h1>


/* mime-types */
static const struct {
    char *ext;
    char *type;
} mimes[] = {
    { "xml",   "application/xml; charset=utf-8" },
    { "xhtml", "application/xhtml+xml; charset=utf-8" },
    { "html",  "text/html; charset=utf-8" },
    { "htm",   "text/html; charset=utf-8" },
    { "css",   "text/css; charset=utf-8" },
    { "txt",   "text/plain; charset=utf-8" },
    { "vtt",   "text/plain; charset=utf-8" },
    { "md",    "text/plain; charset=utf-8" },
    { "c",     "text/plain; charset=utf-8" },
    { "h",     "text/plain; charset=utf-8" },
    { "gz",    "application/x-gtar" },
    { "tar",   "application/tar" },
    { "pdf",   "application/pdf" },
    { "png",   "image/png" },
    { "gif",   "image/gif" },
    { "jpeg",  "image/jpg" },
    { "jpg",   "image/jpg" },
    { "iso",   "application/x-iso9660-image" },
    { "webp",  "image/webp" },
    { "svg",   "image/svg+xml; charset=utf-8" },
    { "flac",  "audio/flac" },
    { "mp3",   "audio/mpeg" },
    { "ogg",   "audio/ogg" },
    { "mp4",   "video/mp4" },
    { "ogv",   "video/ogg" },
    { "webm",  "video/webm" },
};