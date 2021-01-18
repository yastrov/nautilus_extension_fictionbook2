#include <assert.h>
#include <errno.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <zip.h>
#include <stdint.h> /* For numeric (size_t) limits. */
#include <string.h>

#include <glib.h>
#include <gio/gio.h>
#include <nautilus-extension.h>

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
/* Start FB2 only */                 
typedef struct {
    xmlChar *title;
    xmlChar *first_name;
    xmlChar *last_name;
} FB2Info;
        
static int read_from_plain_fb2(const char* filename, FB2Info *info);
static int read_from_zip_fb2(const char *archive, FB2Info *info);
static int parse_xml_from_buffer(char *content, zip_uint64_t uncomp_size, FB2Info *info);
static int process_xml(xmlDocPtr doc, FB2Info *info);

enum FB2_RESULT {
    FB2_RESULT_OK = 0,
    FB2_RESULT_INVALID_FB2,
    FB2_RESULT_CANT_OPEN,
    FB2_RESULT_ZIP_OPEN_FILE_ERR,
    FB2_RESULT_ZIP_READ_FILE_ERR,
    FB2_RESULT_ZIP_CANT_CLOSE,
    FB2_RESULT_UNABLE_PARSE_MEM_BUFF,
    FB2_RESULT_UNABLE_CREATE_XPATH_CONTEXT
};

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
    GList *_ret = NULL;
    column = nautilus_column_new ("FB2Extension::fb2_data_column",
                                  "FB2Extension::fb2_data",
                                  "FB2 Information",
                                  "FictionBook2 Information");
    ret = g_list_append(ret, column);
    _ret = ret;
    column = nautilus_column_new ("FB2Extension::fb2_lastname_column",
                                  "FB2Extension::fb2_lastname",
                                  "FB2 Lastname",
                                  "FictionBook2 Author Lastname");
    _ret = g_list_append(_ret, column);
    column = nautilus_column_new ("FB2Extension::fb2_firsttname_column",
                                  "FB2Extension::fb2_firstname",
                                  "FB2 Firsttname",
                                  "FictionBook2 Author Firstname");
    _ret = g_list_append(_ret, column);
    column = nautilus_column_new ("FB2Extension::fb2_title_column",
                                  "FB2Extension::fb2_title",
                                  "FB2 Title",
                                  "FictionBook2 Title");
    _ret = g_list_append(_ret, column);
    return ret;
}

/* Info interfaces */
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
    FB2Info info;
    UpdateHandle *handle = (UpdateHandle*)data;
    if (!handle->cancelled) {
        char *filename = g_file_get_path(nautilus_file_info_get_location(handle->file));
        int result = read_from_plain_fb2(filename, &info);
        if(result == FB2_RESULT_OK) {
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_data",
                                                     (char*)info.title );
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_title",
                                                    (char*)info.title);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_lastname",
                                                    (char*)info.last_name);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_firstname",
                                                    (char*)info.first_name);
        
            /* Cache the data so that we don't have to read it again */
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_data",
                                    g_strdup((char*)info.title),
                                    g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_title",
                                    g_strdup((char*)info.title),
                                    g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_lastname",
                                    g_strdup((char*)info.last_name),
                                    g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_firstname",
                                    g_strdup((char*)info.first_name),
                                    g_free);
            
            if(info.title != NULL) xmlFree(info.title);
            if(info.first_name != NULL) xmlFree(info.first_name);
            if(info.last_name != NULL) xmlFree(info.last_name);
        } else {
            char *data_s = g_strdup_printf("%s, Code: %d", fb2_errors[result], result);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_data",
                                                     data_s);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_title",
                                                     data_s);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_data",
                                    g_strdup(data_s),
                                    g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_title",
                                    data_s,
                                    g_free);
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
    FB2Info info;
    UpdateHandle *handle = (UpdateHandle*)data;
    if (!handle->cancelled) {
        char *filename = g_file_get_path(nautilus_file_info_get_location(handle->file));
        int result = read_from_zip_fb2(filename, &info);
        if(result == FB2_RESULT_OK) {
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_data",
                                                     (char*)info.title );
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_title",
                                                    (char*)info.title);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_lastname",
                                                    (char*)info.last_name);
            nautilus_file_info_add_string_attribute(handle->file,
                                                    "FB2Extension::fb2_firstname",
                                                    (char*)info.first_name);
        
            /* Cache the data so that we don't have to read it again */
            g_object_set_data_full(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_data",
                                            g_strdup((char*)info.title),
                                            g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_title",
                                            g_strdup((char*)info.title),
                                            g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_lastname",
                                            g_strdup((char*)info.last_name),
                                            g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                            "fb2_extension_fb2_firstname",
                                            g_strdup((char*)info.first_name),
                                            g_free);
            
            if(info.title != NULL) xmlFree(info.title);
            if(info.first_name != NULL) xmlFree(info.first_name);
            if(info.last_name != NULL) xmlFree(info.last_name);
        } else {
            char *data_s = g_strdup_printf("%s, Code: %d", fb2_errors[result], result);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_data",
                                                     data_s);
            nautilus_file_info_add_string_attribute (handle->file,
                                                    "FB2Extension::fb2_title",
                                                     data_s);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_data",
                                    g_strdup(data_s),
                                    g_free);
            g_object_set_data_full(G_OBJECT (handle->file), 
                                    "fb2_extension_fb2_title",
                                    data_s,
                                    g_free);
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
read_from_plain_fb2(const char* filename, FB2Info *info)
{
    xmlDocPtr doc;

    assert(filename);

    /* Load XML document */
    doc = xmlParseFile(filename);
    if (doc == NULL) {
        return(FB2_RESULT_INVALID_FB2);
    }

    int result = process_xml(doc, info);

    /* free the document */
    xmlFreeDoc(doc); 
    
    return(result);
}

static int
read_from_zip_fb2(const char *archive, FB2Info *info)
{
    int result = FB2_RESULT_OK; /* Result for operation */
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
        return FB2_RESULT_ZIP_CANT_OPEN;
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
                        return(FB2_RESULT_ZIP_OPEN_FILE_ERR);
                    }
                    /* Cast to size_t conversion safe (for malloc).*/
                    if(SIZE_MAX < uncomp_size+1) {
                        zip_close(za);
                        return(FB2_RESULT_ZIP_OPEN_FILE_ERR);
                    }
                    /* Conversion from zip_uint64_t to size_t is bad, but no way.*/
                    dataEntry = (char *)g_malloc0((size_t)uncomp_size+1);
                    if(dataEntry != NULL) {
                        fread_len = zip_fread(zf, dataEntry, uncomp_size);
                        if(fread_len < 0) {
                            zip_fclose(zf);
                            free(dataEntry);
                            zip_close(za);
                            return(FB2_RESULT_ZIP_READ_FILE_ERR);
                        }
                        if((zip_uint64_t)fread_len < uncomp_size) {
                            zip_fclose(zf);
                            free(dataEntry);
                            zip_close(za);
                            return(FB2_RESULT_ZIP_READ_FILE_ERR);
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
        return(FB2_RESULT_ZIP_CANT_CLOSE);
    }
    return result;
}

static int
parse_xml_from_buffer(char *content, zip_uint64_t uncomp_size, FB2Info *info)
{
    xmlDocPtr doc;
    assert(content);
    
    doc = xmlReadMemory(content, (size_t)uncomp_size, "fb2.xml", NULL,
            XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
    if (doc == NULL) {
        return(FB2_RESULT_UNABLE_PARSE_MEM_BUFF);
    }

    const int result = process_xml(doc, info);

    /* free the document */
    xmlFreeDoc(doc);
    return(result);
}

static int
process_xml(xmlDocPtr doc, FB2Info *info)
{
    xmlXPathContextPtr xpathCtx; 
    xmlXPathObjectPtr xpathObj;
    xmlXPathObjectPtr xpathObjTitle, xpathObjAuthor, xpathObjAuthorFirtsName, xpathObjAuthorLastName; 

    /* Create xpath evaluation context */
    xpathCtx = xmlXPathNewContext(doc);
    if(xpathCtx == NULL) {
        return(FB2_RESULT_UNABLE_CREATE_XPATH_CONTEXT);
    }
    
    xmlXPathRegisterNs(xpathCtx , BAD_CAST "fb2", BAD_CAST "http://www.gribuser.ru/xml/fictionbook/2.0");
    //xpathObj = xmlXPathEval((xmlChar*)"/fb2:FictionBook/fb2:description/fb2:title-info/fb2:book-title", xpathCtx);
    xpathObj = xmlXPathEval((xmlChar*)"/fb2:FictionBook/fb2:description/fb2:title-info", xpathCtx);
    if (xpathObj->type == XPATH_NODESET) {
        xmlNodePtr node = xpathCtx->node;
        xpathCtx->node = xpathObj->nodesetval->nodeTab[0];
        
        /*Request Title*/
        xpathObjTitle = xmlXPathEval((xmlChar*)"fb2:book-title", xpathCtx);
        if (xpathObjTitle->type == XPATH_NODESET) {
            info->title = xmlXPathCastNodeSetToString(xpathObjTitle->nodesetval);
        }
        xmlXPathFreeObject(xpathObjTitle);
        /* Request Author */
        xpathObjAuthor = xmlXPathEval((xmlChar*)"fb2:author", xpathCtx);
        if (xpathObjAuthor->type == XPATH_NODESET) {
            /* Set fb2:author node for future context */
            xpathCtx->node = xpathObjAuthor->nodesetval->nodeTab[0];
            /* Request first-name */
            xpathObjAuthorFirtsName = xmlXPathEval((xmlChar*)"fb2:first-name", xpathCtx);
            if (xpathObjAuthorFirtsName->type == XPATH_NODESET) {
                info->first_name = xmlXPathCastNodeSetToString(xpathObjAuthorFirtsName->nodesetval);
            }
            xmlXPathFreeObject(xpathObjAuthorFirtsName);
            /* Request last-name */
            xpathObjAuthorLastName = xmlXPathEval((xmlChar*)"fb2:last-name", xpathCtx);
            if (xpathObjAuthorLastName->type == XPATH_NODESET) {
                info->last_name = xmlXPathCastNodeSetToString(xpathObjAuthorLastName->nodesetval);
            }
            xmlXPathFreeObject(xpathObjAuthorLastName); 
        }
        xmlXPathFreeObject(xpathObjAuthor);
        /* Recover context */
        xpathCtx->node = node;
    }
    /* Cleanup of XPath data */
    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx); 
    return (FB2_RESULT_OK);
}
