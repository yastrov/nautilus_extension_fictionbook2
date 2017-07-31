#include <assert.h>
#include <errno.h>

#include <libxml/parser.h>

#include <zip.h>
#include <stdint.h> /* For numeric (size_t) limits. */
#include <string.h>
#ifdef DEBUG
#include <stdio.h>
#endif
#include <glib.h>
#include <gio/gio.h>
#include <libnautilus-extension/nautilus-column-provider.h>
#include <libnautilus-extension/nautilus-info-provider.h>

typedef struct _FB2Extension FB2Extension;
typedef struct _FB2ExtensionClass FB2ExtensionClass;

typedef struct {
    GClosure *update_complete;
    NautilusInfoProvider *provider;
    NautilusFileInfo *file;
    int operation_handle;
    gboolean cancelled;
} UpdateHandle;

struct _FB2Extension
{
    GObject parent_slot;
};

struct _FB2ExtensionClass
{
    GObjectClass parent_slot;
};

static void fb2_extension_class_init    (FB2ExtensionClass *class);
static void fb2_extension_instance_init (FB2Extension      *img);
static GList *fb2_extension_get_columns (NautilusColumnProvider *provider);
static NautilusOperationResult fb2_extension_update_file_info (
                                NautilusInfoProvider *provider,
                                NautilusFileInfo *file,
                                GClosure *update_complete,
                                NautilusOperationHandle **handle);

static void fb2_extension_cancel_update(NautilusInfoProvider *provider,
                                        NautilusOperationHandle *handle);
/* Start FB2 only */
 enum FSM_State {
    INIT,
    BOOK_TITLE_OPENED,
    BOOK_TITLE_END,
    AUTHOR_OPENED,
    AUTHOR_END,
    AUTHOR_FIRSTNAME_OPENED,
    AUTHOR_FIRSTNAME_END,
    AUTHOR_LASTNAME_OPENED,
    AUTHOR_LASTNAME_END,
    GET_AUTH_INFO_COMPLETE,
    STOP
};

#define TITLE_LENGTH 240
#define FIRST_NAME_LENGTH 50
#define LAST_NAME_LENGTH 20
typedef struct {
    char title[TITLE_LENGTH];
    char first_name[FIRST_NAME_LENGTH];
    char last_name[LAST_NAME_LENGTH];
    enum FSM_State my_state;
} FB2Info;
        
static int read_from_zip_fb2(const char *archive, FB2Info *info);
static int parse_xml_from_buffer(char *content, zip_uint64_t uncomp_size, FB2Info *info);

static void make_sax_handler(xmlSAXHandler *SAXHander, void *user_data);
static void OnStartElementNs(
    void *ctx,
    const xmlChar *localname,
    const xmlChar *prefix,
    const xmlChar *URI,
    int nb_namespaces,
    const xmlChar **namespaces,
    int nb_attributes,
    int nb_defaulted,
    const xmlChar **attributes
    );

static void OnEndElementNs(
    void* ctx,
    const xmlChar* localname,
    const xmlChar* prefix,
    const xmlChar* URI
    );
static void OnCharacters(void * ctx,
	const xmlChar * ch,
	int len);

const static char nonFb2[] = "Non FB2 file.";
const static char *fb2_errors[] = {"ok", "Invalid FB2 file.", "can't open zip archive",
                                    "ZIP read error", "ZIP inner file read error",
                                    "can't close zip archive", "Error: unable to parse file from memory buffer",
                                    "Error: unable to create new XPath context"};

gint timeout_plain_fb2_callback(gpointer data);
gint timeout_zip_fb2_callback(gpointer data);
/*end */

/* Interfaces */
static void
fb2_extension_column_provider_iface_init (NautilusColumnProviderIface *iface) {
    iface->get_columns = fb2_extension_get_columns;
    return;
}

static void
fb2_extension_info_provider_iface_init (NautilusInfoProviderIface *iface) {
    iface->update_file_info = fb2_extension_update_file_info;
    iface->cancel_update = fb2_extension_cancel_update;
    return;
}

/* Extension */
static void fb2_extension_class_init(FB2ExtensionClass *class)
{
}

static void fb2_extension_instance_init(FB2Extension *img)
{
}

static GType provider_types[1];

static GType fb2_extension_type;

static void fb2_extension_register_type(GTypeModule *module)
{
        static const GTypeInfo info = {
                                        sizeof(FB2ExtensionClass),
                                        (GBaseInitFunc) NULL,
                                        (GBaseFinalizeFunc) NULL,
                                        (GClassInitFunc) fb2_extension_class_init,
                                        NULL,
                                        NULL,
                                        sizeof (FB2Extension),
                                        0,
                                        (GInstanceInitFunc) fb2_extension_instance_init,
                                        };

        static const GInterfaceInfo column_provider_iface_info = {
            (GInterfaceInitFunc) fb2_extension_column_provider_iface_init,
            NULL,
            NULL
        };

        static const GInterfaceInfo info_provider_iface_info = {
            (GInterfaceInitFunc) fb2_extension_info_provider_iface_init,
            NULL,
            NULL
        };

        fb2_extension_type = g_type_module_register_type(module,
                                                        G_TYPE_OBJECT,
                                                        "FB2Extension",
                                                        &info, 0);

        /* ... add interfaces ... */
        g_type_module_add_interface (module,
                                    fb2_extension_type,
                                    NAUTILUS_TYPE_COLUMN_PROVIDER,
                                    &column_provider_iface_info);

        g_type_module_add_interface (module,
                                     fb2_extension_type,
                                     NAUTILUS_TYPE_INFO_PROVIDER,
                                     &info_provider_iface_info);
}

GType fb2_extension_get_type(void)
{
    return fb2_extension_type;
}

/* Column interfaces */
static GList *fb2_extension_get_columns(NautilusColumnProvider *provider)
{
    NautilusColumn *column;
    GList *ret = NULL;
    column = nautilus_column_new ("FB2Extension::fb2_data_column",
                                  "FB2Extension::fb2_data",
                                  "FB2 Information",
                                  "FictionBook2 Information");
    ret = g_list_append(ret, column);
    column = nautilus_column_new ("FB2Extension::fb2_lastname_column",
                                  "FB2Extension::fb2_lastname",
                                  "FB2 Lastname",
                                  "FictionBook2 Author Lastname");
    ret = g_list_append(ret, column);
    column = nautilus_column_new ("FB2Extension::fb2_firsttname_column",
                                  "FB2Extension::fb2_firstname",
                                  "FB2 Firsttname",
                                  "FictionBook2 Author Firstname");
    ret = g_list_append(ret, column);
    column = nautilus_column_new ("FB2Extension::fb2_title_column",
                                  "FB2Extension::fb2_title",
                                  "FB2 Title",
                                  "FictionBook2 Title");
    ret = g_list_append(ret, column);
    return ret;
}

/* Info interfaces */
static void
fb2_extension_cancel_update(NautilusInfoProvider *provider,
                             NautilusOperationHandle *handle)
{
    UpdateHandle *update_handle = (UpdateHandle*)handle;
    update_handle->cancelled = TRUE;
}

static NautilusOperationResult
fb2_extension_update_file_info (NautilusInfoProvider *provider,
                NautilusFileInfo *file,
                GClosure *update_complete,
                NautilusOperationHandle **handle)
{
    if(nautilus_file_info_is_directory(file))
        return NAUTILUS_OPERATION_COMPLETE;
    char *data = NULL;
    char *dataTitle = NULL;
    char *dataFirstName = NULL;
    char *dataLastName = NULL;
    /* Check if we've previously cached the file info */
    data = g_object_get_data (G_OBJECT (file), "fb2_extension_fb2_data");
    dataTitle = g_object_get_data (G_OBJECT (file), "fb2_extension_fb2_title");
    dataLastName = g_object_get_data (G_OBJECT (file), "fb2_extension_fb2_lastname");
    dataFirstName = g_object_get_data (G_OBJECT (file), "fb2_extension_fb2_firstname");

    /* get and provide the information associated with the column.
       If the operation is not fast enough, we should use the arguments 
       update_complete and handle for asyncrhnous operation. */
    if (!data) {
        char *filename = nautilus_file_info_get_name(file);
        const int len = strlen(filename);
        if(len > 4 && g_strcmp0(&filename[len-4], ".fb2") == 0) {
            /* Plain FB2 */
            UpdateHandle *update_handle = g_new0 (UpdateHandle, 1);
            update_handle->update_complete = g_closure_ref(update_complete);
            update_handle->provider = provider;
            update_handle->file = g_object_ref (file);
            //g_timeout_add (1, 
            g_idle_add(
                timeout_plain_fb2_callback, 
                update_handle);
            *handle = update_handle;
            g_free(filename);
            return NAUTILUS_OPERATION_IN_PROGRESS;      
        } else {
            if(len > 8 && g_strcmp0(&filename[len-8], ".fb2.zip") == 0) {
                /*Zipped FB2*/
                UpdateHandle *update_handle = g_new0 (UpdateHandle, 1);
                update_handle->update_complete = g_closure_ref(update_complete);
                update_handle->provider = provider;
                update_handle->file = g_object_ref (file);
                //g_timeout_add (1, 
                g_idle_add(
                        timeout_zip_fb2_callback, 
                        update_handle);
                *handle = update_handle;
                
                g_free(filename);
                return NAUTILUS_OPERATION_IN_PROGRESS;
            } else {
                /* Other filetype */
                nautilus_file_info_add_string_attribute(file,
                                                        "FB2Extension::fb2_data",
                                                        nonFb2);
                nautilus_file_info_add_string_attribute(file,
                                                        "FB2Extension::fb2_title",
                                                        nonFb2);
            }
        }
        g_free(filename);
        return NAUTILUS_OPERATION_COMPLETE;
    }
    nautilus_file_info_add_string_attribute(file,
                                            "FB2Extension::fb2_data",
                                            data);
    nautilus_file_info_add_string_attribute(file,
                                            "FB2Extension::fb2_title",
                                            dataTitle);
    nautilus_file_info_add_string_attribute(file,
                                            "FB2Extension::fb2_lastname",
                                            dataLastName);
    nautilus_file_info_add_string_attribute(file,
                                            "FB2Extension::fb2_firstname",
                                            dataFirstName);
    return NAUTILUS_OPERATION_COMPLETE;
}

/* Extension initialization */
void nautilus_module_initialize (GTypeModule  *module)
{
    fb2_extension_register_type(module);
    provider_types[0] = fb2_extension_get_type();
    xmlInitParser();
    LIBXML_TEST_VERSION
}

void nautilus_module_shutdown(void)
{
    /* Any module-specific shutdown */
    xmlCleanupParser();
}

void nautilus_module_list_types (const GType **types, int *num_types)
{
    *types = provider_types;
    *num_types = G_N_ELEMENTS (provider_types);
}
/* Callback for async */
gint
timeout_plain_fb2_callback(gpointer data)
{
	#ifdef DEBUG
    fprintf(stderr, "Could not read config file\n");
    #endif
    UpdateHandle *handle = (UpdateHandle*)data;
    if (!handle->cancelled) {
        #ifdef DEBUG
        fprintf(stderr, "SAX handler create next!\n");
        #endif
        char *filename = g_file_get_path(nautilus_file_info_get_location(handle->file));
        
        FB2Info info;
        xmlSAXHandler my_sax_handler;
	    make_sax_handler(&my_sax_handler, &info);
	    const int result = xmlSAXUserParseFile(&my_sax_handler, NULL, filename);

        if(result >= 0) {
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_data",
                                                     info.title );
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_title",
                                                     info.title);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_lastname",
                                                    info.last_name);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_firstname",
                                                    info.first_name);
        
            /* Cache the data so that we don't have to read it again */
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_data",
                                    info.title);
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_title",
                                    info.title);
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_lastname",
                                    info.last_name);
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_firstname",
                                    info.first_name);
        } else {
            char *data_s = g_strdup_printf("%s, Code: %d", fb2_errors[result], result);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_data",
                                                     data_s);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_title",
                                                     data_s);
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_data",
                                    data_s);
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_title",
                                    data_s);
            g_free(data_s);
        }
        g_free(filename);
    }
    
    nautilus_info_provider_update_complete_invoke
                        (handle->update_complete,
                         handle->provider,
                         (NautilusOperationHandle*)handle,
                         NAUTILUS_OPERATION_COMPLETE);
    /* We're done with the handle */
    g_closure_unref (handle->update_complete);
    g_object_unref (handle->file);
    g_free (handle);
    return 0;
}

gint
timeout_zip_fb2_callback(gpointer data)
{
    UpdateHandle *handle = (UpdateHandle*)data;
    if (!handle->cancelled) {
        char *filename = g_file_get_path(nautilus_file_info_get_location(handle->file));
        FB2Info info;
		const int result = read_from_zip_fb2(filename, &info);
        if(result == 0) {
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_data",
                                                     info.title );
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_title",
                                                     info.title);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_lastname",
                                                     info.last_name);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_firstname",
                                                     info.first_name);
        
            /* Cache the data so that we don't have to read it again */
            g_object_set_data(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_data",
                                            info.title);
            g_object_set_data(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_title",
                                            info.title);
            g_object_set_data(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_lastname",
                                            info.last_name);
            g_object_set_data(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_firstname",
                                            info.first_name);
        } else {
            char *data_s = g_strdup_printf("%s, Code: %d", fb2_errors[result], result);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_data",
                                                     data_s);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_title",
                                                     data_s);
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_data",
                                    data_s);
            g_object_set_data(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_title",
                                    data_s);
        }
        g_free(filename);
    }
    
    nautilus_info_provider_update_complete_invoke
                                                (handle->update_complete,
                                                 handle->provider,
                                                 (NautilusOperationHandle*)handle,
                                                 NAUTILUS_OPERATION_COMPLETE);
    /* We're done with the handle */
    g_closure_unref (handle->update_complete);
    g_object_unref (handle->file);
    g_free (handle);
    return 0;
}
/* Fb2 */

static int
read_from_zip_fb2(const char *archive, FB2Info *info)
{
    int result = 0; /* Result for operation */
    /* Zip error */
    int err = 0;
    char errbuf[100];
    char *dataEntry;
    /* Zip */
    struct zip *za;
    struct zip_file *zf;
    struct zip_stat sb;
    /* For files in zip */
    zip_int64_t num64; /* Number of files */
    zip_uint64_t i;    /* Counter, for 0..num64 */
    zip_uint64_t uncomp_size; /* Zize of uncompressed file */
    zip_int64_t fread_len; /* Really read bytes from zipped file. */
    int len;
    if ((za = zip_open(archive, 0, &err)) == NULL) {
        zip_error_to_str(errbuf, sizeof(errbuf), err, errno);
        return 2;
    }
    num64 = zip_get_num_entries(za, 0);
    for (i = 0; i < num64; ++i) {
        zip_stat_init(&sb);
        if (zip_stat_index(za, i, 0, &sb) == 0) {
            len = strlen(sb.name);
            uncomp_size = sb.size;
            if (sb.name[len-1] == '/') {
                //safe_create_dir(sb.name);
                ;
            } else {
                if(len > 4 && g_strcmp0(&sb.name[len-4], ".fb2") == 0) {
                    zf = zip_fopen_index(za, i, 0);
                    if(!zf) {
                        zip_close(za);
                        return(3);
                    }
                    /* Cast to size_t conversion safe (for malloc).*/
                    if(SIZE_MAX < uncomp_size+1) {
                        zip_close(za);
                        return(3);
                    }
                    /* Conversion from zip_uint64_t to size_t is bad, but no way.*/
                    dataEntry = (char *)g_malloc0((size_t)uncomp_size+1);
                    if(dataEntry != NULL) {
                        fread_len = zip_fread(zf, dataEntry, uncomp_size);
                        if(fread_len < 0) {
                            zip_fclose(zf);
                            free(dataEntry);
                            zip_close(za);
                            return(4);
                        }
                        if((zip_uint64_t)fread_len < uncomp_size) {
                            zip_fclose(zf);
                            free(dataEntry);
                            zip_close(za);
                            return(4);
                        }
                        result = parse_xml_from_buffer(dataEntry, uncomp_size, info);
                        zip_fclose(zf);
                        g_free(dataEntry);
                        break;
                    }
                }
            }
        }
    }
    if (zip_close(za) == -1) {
        return(5);
    }
    return result;
}

static int
parse_xml_from_buffer(char *content, zip_uint64_t uncomp_size, FB2Info *info)
{
    int my_size;
    if(INT32_MAX < uncomp_size+1) {
        my_size = INT32_MAX -1;
    }
    else {
        my_size = (int)uncomp_size;   
    }

    make_sax_handler(&my_sax_handler, info);
    const int result = xmlSAXUserParseMemory(&my_sax_handler, NULL, content, my_size);

    return(result);
}

static void make_sax_handler(xmlSAXHandler *SAXHander,
    void *user_data)
{
    SAXHander->initialized = XML_SAX2_MAGIC;
    SAXHander->startElementNs = OnStartElementNs;
    SAXHander->endElementNs = OnEndElementNs;
    SAXHander->characters = OnCharacters;
    SAXHander->_private = user_data;
}

static inline int 
min(int a, int b)
{
    if(a < b)
        return a;
    return b;
}

static void
OnCharacters(void * ctx,
    const xmlChar * ch,
    int len)
{
	#ifdef DEBUG
	fprintf(stderr, "Event: OnCharacters!\n");
	#endif
    xmlSAXHandlerPtr handler = ((xmlParserCtxtPtr)ctx)->sax;
    FB2Info *info = (FB2Info *)(handler->_private);
    switch (info->my_state) {
    case AUTHOR_FIRSTNAME_OPENED: {
        const int len2 = min(len, FIRST_NAME_LENGTH);
        strncpy(info->first_name, (const char *)ch, len);
        info->my_state = AUTHOR_FIRSTNAME_END;
        }
        break;
    case AUTHOR_LASTNAME_OPENED: {
        const int len2 = min(len, LAST_NAME_LENGTH);
        strncpy(info->last_name, (const char *)ch, len2);
        info->my_state = AUTHOR_LASTNAME_END;
        }
        break;
    case BOOK_TITLE_OPENED: {
        const int len2 = min(len, TITLE_LENGTH);
        strncpy(info->title, (const char *)ch, len2);
        info->my_state = BOOK_TITLE_END;
        }
        break;
    default:
        break;
    }
}

static void
OnStartElementNs(
    void *ctx,
    const xmlChar *localname,
    const xmlChar *prefix,
    const xmlChar *URI,
    int nb_namespaces,
    const xmlChar **namespaces,
    int nb_attributes,
    int nb_defaulted,
    const xmlChar **attributes
    )
{
	#ifdef DEBUG
	fprintf (stderr, "Event: OnStartElementNs!\n");
	#endif
    xmlSAXHandlerPtr handler = ((xmlParserCtxtPtr)ctx)->sax;
    FB2Info *info = (FB2Info *)(handler->_private);
    info->my_state = INIT;
    if (g_strcmp0((const char*)localname, "author") == 0) {
        info->my_state = AUTHOR_OPENED;
        return;
    }
    if (g_strcmp0((const char*)localname, "first-name") == 0) {
        info->my_state = AUTHOR_FIRSTNAME_OPENED;
        return;
    }
    if (g_strcmp0((const char*)localname, "last-name") == 0) {
        info->my_state = AUTHOR_LASTNAME_OPENED;
        return;
    }
    if (g_strcmp0((const char*)localname, "book-title") == 0) {
        info->my_state = BOOK_TITLE_OPENED;
        return;
    }
}

static void
OnEndElementNs(
    void* ctx,
    const xmlChar* localname,
    const xmlChar* prefix,
    const xmlChar* URI
    )
{
	#ifdef DEBUG
	fprintf (stderr, "Event: OnEndElementNs!\n");
	#endif
    xmlSAXHandlerPtr handler = ((xmlParserCtxtPtr)ctx)->sax;
    FB2Info *info = (FB2Info *)(handler->_private);
    if (info->my_state == BOOK_TITLE_END) {
        xmlStopParser(ctx);
        return;
    }
    if (g_strcmp0((const char*)localname, "title-info") == 0)
        xmlStopParser(ctx);
}
