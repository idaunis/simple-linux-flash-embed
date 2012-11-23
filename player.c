#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

//#include "npupp.h"
#include "npapi.h"
#include "npfunctions.h"

//#ifdef __i386__
#define FLASH_PLUGIN_SO "./flash/libflashplayer32.so"
//#else
//#define FLASH_PLUGIN_SO "./flash/libflashplayer64.so"
//#endif

#define PR_TRUE	1
#define PR_FALSE 0

#define WINDOW_XSIZE 1600/2
#define WINDOW_YSIZE 900/2

#define NO_IDENTIFIER		((NPIdentifier)0)
#define	SPECIAL_IDENTIFIER  0x0FEEBBCC
#define SPECIAL_METHOD_NAME "swhxCall"

#define FLASH_REQUEST		"__flash__request"
#define FSCMD			"_DoFSCommand"
#define INVOKE_RESPONSE "<invoke name=\"%s\" returntype=\"javascript\"><arguments><null/></arguments></invoke>"

typedef intptr_t int_val;

void *flash_plugin_handle;

NPNetscapeFuncs browserFuncs;
NPPluginFuncs pluginFuncs;

GtkWidget *main_window;

char* URL;
NPStream * stream;
const char * uagent = "Mozilla/5.0 (X11; U; Linux x86_64; en-US; rv:1.9.2.13) Gecko/20101206 Ubuntu/10.10 (maverick) Firefox/3.6.13";

NPError (*iNP_Initialize)(NPNetscapeFuncs* bFuncs, NPPluginFuncs* pFuncs);
NPError (*iNP_Shutdown)();
char* (*iNP_GetMIMEDescription)();

// --- Property IDs

static char **np_ids = NULL;
static int_val np_id_count = 0;

static NPIdentifier resolveNPId( const char *id ) {
	int_val i;
	for(i=0;i<np_id_count;i++)
		if( strcmp(np_ids[i],id) == 0 )
			return (NPIdentifier)(i+1);
	if( strcmp(id,SPECIAL_METHOD_NAME) == 0 )
		return (NPIdentifier)SPECIAL_IDENTIFIER;
	return NO_IDENTIFIER;
}

static NPIdentifier addNPId( const char *id ) {
	NPIdentifier newid = resolveNPId(id);
	if( newid == NO_IDENTIFIER ) {
		np_id_count++;
		printf("New npid added: %i == %s\n",np_id_count, id);
		np_ids = realloc(np_ids,np_id_count*sizeof(char*));
		np_ids[np_id_count-1] = strdup(id);
		return (NPIdentifier)np_id_count;
	}
	return newid;
}

static const char *getNPId( NPIdentifier id ) {
	int_val index = ((int_val)id)-1;
	if( index >= 0 && index < np_id_count )
		return np_ids[index];
	if( id == (NPIdentifier)SPECIAL_IDENTIFIER )
		return SPECIAL_METHOD_NAME;
	return NULL;
}

static int matchNPId(NPIdentifier id, const char *str) {
	const char *strid = getNPId(id);
	return ( strid != NULL && strcmp(strid,str) == 0 );
}

void freeNPIds() {
	while( np_id_count )
		free(np_ids[--np_id_count]);
	free(np_ids);
}

static bool NPN_InvokeProc( NPP npp, NPObject *npobj, NPIdentifier methodName, const NPVariant *args, uint32_t argCount, NPVariant *result ) ;
static bool NPN_InvokeDefaultProc( NPP npp, NPObject *npobj, const NPVariant *args, uint32_t argCount, NPVariant *result ) ;
static bool NPN_GetPropertyProc( NPP npp, NPObject *npobj, NPIdentifier propertyName, NPVariant *result ) ;
static bool NPN_SetPropertyProc( NPP npp, NPObject *npobj, NPIdentifier propertyName, const NPVariant *value ) ;
static bool NPN_RemovePropertyProc( NPP npp, NPObject *npobj, NPIdentifier propertyName ) ;
static bool NPN_HasPropertyProc( NPP npp, NPObject *npobj, NPIdentifier propertyName ) ;
static bool NPN_HasMethodProc( NPP npp, NPObject *npobj, NPIdentifier methodName ) ;

static NPObject *NPN_CreateObjectProc( NPP npp, NPClass *aClass );
static NPObject *NPN_RetainObjectProc( NPObject *npobj );

// Window class;
static NPClass __gen_class =
	{ NP_CLASS_STRUCT_VERSION
	, (NPAllocateFunctionPtr) malloc
	, (NPDeallocateFunctionPtr) free
	, 0
	, (NPHasMethodFunctionPtr) NPN_HasMethodProc
	, (NPInvokeFunctionPtr) NPN_InvokeProc
	, (NPInvokeDefaultFunctionPtr)NPN_InvokeDefaultProc
	, (NPHasPropertyFunctionPtr) NPN_HasPropertyProc
	, (NPGetPropertyFunctionPtr) NPN_GetPropertyProc
	, (NPSetPropertyFunctionPtr) NPN_SetPropertyProc
	, (NPRemovePropertyFunctionPtr) NPN_RemovePropertyProc
	};
static NPObject __window = { &__gen_class, 1 };
static NPObject __location = { &__gen_class, 1};
static NPObject __top = { &__gen_class, 1 };
static NPObject __top_location = { &__gen_class, 1 };

static void traceObjectOnCall(const char *f, NPObject *o){
	if (o == &__top) printf("DOM object 'top': %s\n",f);
	else if (o == &__window) printf("DOM object 'window': %s\n",f);
	else if (o == &__location) printf("DOM object 'location': %s\n",f);
	else if (o == &__top_location) printf("DOM object 'top.location': %s\n",f);
}

static void checkError(const char* str, NPError err) {
if(err == NPERR_NO_ERROR)
printf("[+] %s: success\n", str);
else
printf("[-] %s: failed (%d)\n", str, err);
fflush (stdout);
}

void* loadFlashPluginSo() {
	void *handle;

	handle = dlopen(FLASH_PLUGIN_SO, RTLD_LAZY | RTLD_LOCAL);
	if(!handle) {
		fprintf(stderr, "[-] error loading libflasplayer.so: %s\n", dlerror());
		exit(1);
	}	
	puts("[+] loaded libflashplayer.so");
	return handle;
}

void* loadSymbol(void *handle, const char *name) {
	char *error;
	void *ret;

	ret = dlsym(handle, name);

	if((error = dlerror()) != NULL) {
		fprintf(stderr, "[-] error loading symbol %s: %s\n", name, error);
		exit(1);
	} else {
		printf("[+] loaded symbol %s, address: %p\n", name, ret);
	}
	return ret;
}

void loadNPEntryPoints(void *handle) {
	iNP_Initialize=(NPError (*)(NPNetscapeFuncs* bFuncs, NPPluginFuncs* pFuncs))loadSymbol(handle, "NP_Initialize");
	iNP_Shutdown=(NPError (*)())loadSymbol(handle, "NP_Shutdown");
	iNP_GetMIMEDescription = (char*(*)())loadSymbol(handle,"NP_GetMIMEDescription");
}

void printBrowserEntrypoints(NPNetscapeFuncs* pFuncs) {
printf("[*] NPN struct:\n");
printf("\t- NPN_Evaluate: %p\n", pFuncs->evaluate);
}

void printPluginEntrypoints(NPPluginFuncs* pFuncs) {
printf("[*] NPP struct:\n");
printf("\t- NPP_size: %8d\n",pFuncs->size);
printf("\t- NPP_version: %8d\n",pFuncs->version);
printf("\t- NPP_NewProcPtr: %p\n", pFuncs->newp);
printf("\t- NPP_DestroyProcPtr: %p\n", pFuncs->destroy);
printf("\t- NPP_SetWindowProcPtr: %p\n", pFuncs->setwindow);
printf("\t- NPP_NewStreamProcPtr: %p\n", pFuncs->newstream);
printf("\t- NPP_DestroyStreamProcPtr: %p\n", pFuncs->destroystream);
printf("\t- NPP_StreamAsFileProcPtr: %p\n", pFuncs->asfile);
printf("\t- NPP_WriteReadyProcPtr: %p\n", pFuncs->writeready);
printf("\t- NPP_WriteProcPtr: %p\n", pFuncs->write);
printf("\t- NPP_PrintProcPtr: %p\n", pFuncs->print);
printf("\t- NPP_HandleEventProcPtr: %p\n", pFuncs->event);
printf("\t- NPP_URLNotifyProcPtr: %p\n", pFuncs->urlnotify);
printf("\t- javaClass: %p\n", pFuncs->javaClass);
printf("\t- NPP_GetValueProcPtr: %p\n", pFuncs->getvalue);
printf("\t- NPP_SetValueProcPtr: %p\n", pFuncs->setvalue);
}

NPError NPN_SetValueProc(NPP instance, NPPVariable variable, void *value)
{
	switch(variable) {
		case NPPVpluginWindowBool:
			printf( "NPPVpluginWindowBool - %p\n", value);
			break;
		default:
			printf( "SetValue %i\n", variable );
			break;
	}
	return NPERR_NO_ERROR;
}

NPError NPN_GetValueProc(NPP instance, NPNVariable variable, void *ret_value) {

printf("[D] NPN_GetValueProc instance:%p, variable:%d, abi_mask:%d\n", instance, variable, 0);

switch (variable) {
	case NPNVSupportsXEmbedBool:
		*((int*)ret_value)= PR_TRUE;
		break;
	//Unix and solaris fix
	case NPNVToolkit:
		*((int*)ret_value)= NPNVGtk2;
		break;
	case NPNVnetscapeWindow:
		*((int*)ret_value)= PR_TRUE;
		break;
	default:
		*((int*)ret_value)=PR_FALSE;
		break;
	}
	return NPERR_NO_ERROR;
}

const char* NPN_UserAgentProc(NPP instance) {
   // printf("[D] NPN_UserAgentProc instance:%p\n", instance);
    return uagent;
}

static void Status_( NPP instance, const char* message ) {
	printf( "Status" );
}

uint32_t MemFlush( uint32_t size ) {
	printf( "MemFlush %i", size );
	return 0;
}

void ReloadPlugins( NPBool reloadPages ) {
	printf( "ReloadPlugins %s" , reloadPages?"TRUE":"0" );
}

void * GetJavaEnv(void) {
	printf( "GetJavaEnv" );
	return NULL;
}

void * GetJavaPeer( NPP instance ) {
	printf( "GetJavaPeer" );
	return NULL;
}

NPIdentifier NPN_GetStringIdentifierProc(const NPUTF8* name) {
    printf("[D] GetStringIdentifier: %s\n", name);
    //return (NPIdentifier)0x41424344; //Unique
    return addNPId(name);
}

void GetStringIdentifiers( const NPUTF8 **names, int32_t nameCount, NPIdentifier *identifiers ) {
}

NPIdentifier GetIntIdentifier( int32_t intid ) {
	return NO_IDENTIFIER;
}

bool IdentifierIsString( NPIdentifier id ) {
	return getNPId(id) != NULL;
}

NPUTF8* UTF8FromIdentifier( NPIdentifier identifier ) {
	const char *result = getNPId(identifier);
	return result ? strdup(result) : NULL;
}

int32_t IntFromIdentifier( NPIdentifier id ) {
	return 0;
}
/*
static gboolean button_handler (GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    printf("[!] button\n");
    return TRUE;
}*/

static gboolean plug_removed_cb (GtkWidget *widget, gpointer data) {
    printf("[!] plug_removed_cb\n");
    return TRUE;
}

static void socket_unrealize_cb(GtkWidget *widget, gpointer data) {
    printf("[!] socket_unrealize_cb\n");
}

static NPWindow * npwindow_construct (GtkWidget *widget) {
	NPWindow *npwindow;
	NPSetWindowCallbackStruct *ws_info = NULL;

	GdkWindow *parent_win = widget->window;

	GtkWidget *socketWidget = gtk_socket_new();
	gtk_widget_set_parent_window(socketWidget, parent_win);

	g_signal_connect(socketWidget, "plug_removed", G_CALLBACK(plug_removed_cb), NULL);
	g_signal_connect(socketWidget, "unrealize", G_CALLBACK(socket_unrealize_cb), NULL);
	g_signal_connect(socketWidget, "destroy", G_CALLBACK(gtk_widget_destroyed), &socketWidget);

	gpointer user_data = NULL;
	gdk_window_get_user_data(parent_win, &user_data);

	GtkContainer *container = GTK_CONTAINER(user_data);
	gtk_container_add(container, socketWidget);
	gtk_widget_realize(socketWidget);

	GtkAllocation new_allocation;
	new_allocation.x = 0;
	new_allocation.y = 0;
	new_allocation.width = WINDOW_XSIZE;
	new_allocation.height = WINDOW_YSIZE;
	gtk_widget_size_allocate(socketWidget, &new_allocation);

	gtk_widget_show(socketWidget);
	gdk_flush();

	GdkNativeWindow ww = gtk_socket_get_id(GTK_SOCKET(socketWidget));
	GdkWindow *w = gdk_window_lookup(ww);

	npwindow = (NPWindow*) malloc (sizeof (NPWindow));
	npwindow->window = (void*)(unsigned long)ww;
	npwindow->x = 0;
	npwindow->y = 0;
	npwindow->width = WINDOW_XSIZE;
	npwindow->height = WINDOW_YSIZE;

	ws_info = (NPSetWindowCallbackStruct*) malloc(sizeof (NPSetWindowCallbackStruct));
	ws_info->type = NP_SETWINDOW;
	ws_info->display = GDK_WINDOW_XDISPLAY(w);
	ws_info->colormap = GDK_COLORMAP_XCOLORMAP(gdk_drawable_get_colormap(w));
	GdkVisual* gdkVisual = gdk_drawable_get_visual(w);
	ws_info->visual = GDK_VISUAL_XVISUAL(gdkVisual);
	ws_info->depth = gdkVisual->depth;

	npwindow->ws_info = ws_info;
	npwindow->type = NPWindowTypeWindow;

	return npwindow;
}

static NPStream * npstream_construct() {
    NPStream *stream = (NPStream *) malloc(sizeof(NPStream));
    
    stream->url=strdup(URL);
    stream->ndata = 0;
    stream->end = 99782;
    stream->lastmodified= 1201822722;
    stream->notifyData = 0x00000000;
    stream->headers = (char*)malloc(200);

    return stream;
}

bool NPN_InvokeProc( NPP npp, NPObject *npobj, NPIdentifier npid, const NPVariant *args, uint32_t argCount, NPVariant *result ) {

	traceObjectOnCall(__FUNCTION__,npobj);

	if( matchNPId(npid,FLASH_REQUEST) && argCount == 1 && args[0].type == NPVariantType_String ) {
		return 1;
	}
	if( matchNPId(npid,FLASH_REQUEST) && argCount == 3 &&
		args[0].type == NPVariantType_String &&
		args[1].type == NPVariantType_String &&
		args[2].type == NPVariantType_String
	) {
		return 1;
	}

	if( matchNPId(npid,"_DoFSCommand") && argCount == 2 && args[0].type == NPVariantType_String && args[1].type == NPVariantType_String ) {
		printf("[D] FSCOMMAND\n");
		return 1;
	}

	if( npobj == &__top_location ) {
		if( matchNPId(npid,"toString") ) {
			result->type = NPVariantType_String;
			// "chrome://global/content/console.xul" is what Firefox returns for 'top.location.toString()';
			result->value.stringValue.UTF8Characters = strdup("chrome://global/content/console.xul");
			result->value.stringValue.UTF8Length = (int)strlen(result->value.stringValue.UTF8Characters);
			printf("[D] Returned %s\n", result->value.stringValue.UTF8Characters);
		}
		return 1;
	}
	//On OSX, Flash retreives locations by injected functions:
	if( matchNPId(npid,"__flash_getWindowLocation") ) {
		// return the location object:
		result->type = NPVariantType_Object;
		result->value.objectValue = &__location;
		NPN_RetainObjectProc(&__location);
		return 1;
	}
	if( matchNPId(npid,"__flash_getTopLocation") ) {
		// return the top_location object:
		result->type = NPVariantType_Object;
		result->value.objectValue = &__top_location;
		NPN_RetainObjectProc(&__top_location);
		return 1;
	}

    return 0;
}

static bool NPN_InvokeDefaultProc( NPP npp, NPObject *npobj, const NPVariant *args, uint32_t argCount, NPVariant *result ) {
	return 0;
}

static bool NPN_GetPropertyProc( NPP npp, NPObject *npobj, NPIdentifier npid, NPVariant *result ) {

	if (npobj == &__window) {
		if( matchNPId(npid,"location") ) {
			result->type = NPVariantType_Object;
			result->value.objectValue = &__location;
			NPN_RetainObjectProc(&__location);
			return 1;
		}
		if( matchNPId(npid,"top") ) {
			result->type = NPVariantType_Object;
			result->value.objectValue = &__top;
			NPN_RetainObjectProc(&__top);
			return 1;
		}
	} else if (npobj == &__top) {
		if ( matchNPId(npid,"location") ) {
			result->type = NPVariantType_Object;
			result->value.objectValue = &__top_location;
			NPN_RetainObjectProc(&__top_location);
			return 1;
		}
	}
	return 0;
}

static bool NPN_SetPropertyProc( NPP npp, NPObject *npobj, NPIdentifier propertyName, const NPVariant *value ) {
	return 0;
}

static bool NPN_RemovePropertyProc( NPP npp, NPObject *npobj, NPIdentifier propertyName ) {
	return 0;
}

static bool NPN_HasPropertyProc( NPP npp, NPObject *npobj, NPIdentifier propertyName ) {
	return 0;
}

static bool NPN_HasMethodProc( NPP npp, NPObject *npobj, NPIdentifier methodName ) {
	return 0;
}

static int unescape( char *str, int *ssize ) {
	int k = 0, esc = 0;
	// UNESCAPE the string
	while( k < *ssize ) {
		if( !esc ) {
			if( str[k++] == '\\' )
				esc = 1;
		} else {
			char c;
			switch( str[k] ) {
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case 'r': c = '\r'; break;
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			default:
				return 0;
			}
			(*ssize)--;
			memcpy(str+k,str+k+1,*ssize-k);
			str[k-1] = c;
			esc = 0;
		}
	}
	str[*ssize] = 0;
	return 1;
}

static const char *end_of_string( const char *send ) {
	int esc = 0;
	while( *send ) {
		switch( *send ) {
		case '"':
			if( !esc )
				return send;
			esc = 0;
			break;
		case '\\':
			esc = !esc;
			break;
		default:
			esc = 0;
			break;
		}
		send++;
	}
	return NULL;
}

#define JS_CALL_START "try { __flash__toXML("
#define JS_RESULT_START "var __flash_temp = \""

bool Evaluate( NPP npp, NPObject *npobj, NPString *script, NPVariant *result ) {
	printf( "Evaluate %s\n", script->UTF8Characters );
	if( memcmp(script->UTF8Characters,JS_CALL_START,strlen(JS_CALL_START)) == 0 ) {
		const char *p = script->UTF8Characters + strlen(JS_CALL_START);
		const char *s = p;
		const char *send;
		while( *s && *s != '(' )
			s++;
		if( !*s || s[1] != '"' )
			return 0;
		s += 2;
		send = end_of_string(s);
		if( send == NULL || send[1] != ')' )
			return 0;
		{
			int isize = (int)(s - p) - 2;
			int ssize = (int)(send - s);
			char *ident = (char*)malloc(isize+1);
			char *str = (char*)malloc(ssize+1);
			memcpy(ident,p,isize);
			memcpy(str,s,ssize);
			ident[isize] = 0;
			if( !unescape(str,&ssize) ) {
				free(ident);
				free(str);
				return 0;
			}
			// CALLBACK
			{				
				printf("CALLBACK\n");
				int size = 0;
				const char *res = "callback";
				free(ident);
				free(str);
				if( res == NULL )
					return 0;
				result->type = NPVariantType_String;
				result->value.stringValue.UTF8Characters = strdup(res);
				result->value.stringValue.UTF8Length = size;
				return 1;
			}
		}
	}
	if( memcmp(script->UTF8Characters,JS_RESULT_START,strlen(JS_RESULT_START)) == 0 ) {
		const char *s = script->UTF8Characters + strlen(JS_RESULT_START);
		const char *send = end_of_string(s);
		char *str;
		int ssize;
		if( send == NULL || send[1] != ';' )
			return 0;
		ssize = (int)(send - s);
		str = (char*)malloc(ssize+1);
		memcpy(str,s,ssize);
		if( !unescape(str,&ssize) ) {
			free(str);
			return 0;
		}
		result->type = NPVariantType_String;
		result->value.stringValue.UTF8Characters = str;
		result->value.stringValue.UTF8Length = ssize;
		return 1;
	}
	result->type = NPVariantType_Void;
	return 1;
}

void NPN_ReleaseObjectProc(NPObject *npobj) {
	if( npobj == NULL )
		return;
	npobj->referenceCount--;
	if( npobj->referenceCount != 0 )
		return;
	if( npobj->_class->deallocate ) {
		npobj->_class->deallocate(npobj);
		return;
	}
	if( npobj->_class->invalidate )
		npobj->_class->invalidate(npobj);
	free(npobj);
}

NPObject* NPN_CreateObjectProc(NPP npp, NPClass *aClass) {
	NPObject *o;
	if( aClass->allocate )
		o = aClass->allocate(npp,aClass);
	else
		o = (NPObject*)malloc(sizeof(NPObject));
	o->_class = aClass;
	o->referenceCount = 1;
	return o;
}

NPObject* NPN_RetainObjectProc(NPObject *npobj) {
	if( npobj == NULL )
		return NULL;
	npobj->referenceCount++;
	return npobj;
}

void NPN_ReleaseVariantValueProc(NPVariant *variant) {
	//printf("ReleaseVariantValueProc\n");
	switch( variant->type ) {
		case NPVariantType_Null:
		case NPVariantType_Void:
			break;
		case NPVariantType_String:
			free( (char*)variant->value.stringValue.UTF8Characters );
			variant->type = NPVariantType_Void;
			break;
		case NPVariantType_Object:
			NPN_ReleaseObjectProc(variant->value.objectValue);
			variant->type = NPVariantType_Void;
			break;
		default:
			break;
	}
}

void SetException( NPObject *npobj, const NPUTF8 *message ) {
	printf("SetException %s\n",message);
}

NPError NPN_GetURLNotifyProc(NPP instance, const char* url, const char* window, void* notifyData) {
	printf("[D] NPN_GetURLNotifyProc:%p, url: %s, window: %s\n", instance, url, window);
	if (window && strlen(window)==6 && memcmp("_blank",window,6)==0) {
		//system_launch_url(url);
		pluginFuncs.urlnotify(instance,url,NPRES_DONE,notifyData);
	} else if( memcmp(url,"javascript:",11) == 0 ) {
		NPStream s;
		uint16_t stype;
		int success;
		memset(&s,0,sizeof(NPStream));
		s.url = strdup(url);
		success = (pluginFuncs.newstream(instance,"text/html",&s,0,&stype) == NPERR_NO_ERROR);		
		if( success ) {
			int pos = 0;
			int size;
			char buf[256];
			sprintf(buf,"%X__flashplugin_unique__",(int_val)instance);
			size = (int)strlen(buf);
			s.end = size;
			while( pos < size ) {
				int len = pluginFuncs.writeready(instance,&s);
				if( len <= 0 )
					break;
				if( len > size - pos )
					len = size - pos;
				len = pluginFuncs.write(instance,&s,pos,len,buf+pos);
				if( len <= 0 )
					break;
				pos += len;
			}
			success = (pos == size);
		}
		pluginFuncs.urlnotify(instance,url,success?NPRES_DONE:NPRES_NETWORK_ERR,notifyData);
		pluginFuncs.destroystream(instance,&s,NPRES_DONE);
		free((void*)s.url);
	} else {

		NPStream stream;
		uint16_t stype;// = NP_NORMAL;

		memset(&stream,0,sizeof(NPStream));
		stream.url = strdup(url);
		stream.notifyData = notifyData;

		pluginFuncs.newstream(instance,"application/x-shockwave-flash",&stream, 0, &stype);
		//fprintf(stderr, "NPP: %p URL: %s\n", instance, url);

		FILE *pp;
		char buffer[8192];
		pp = fopen(url,"rb");
		int len;

		while((len=fread(buffer, 1, sizeof(buffer), pp)) != 0) {
			pluginFuncs.writeready(instance, &stream);
			pluginFuncs.write(instance, &stream, 0, len, buffer);
		}

		fclose(pp);
		pluginFuncs.destroystream(instance, &stream, NPRES_DONE);

		free((void*)stream.url);
		//pluginFuncs.urlnotify(instance, url, NPRES_DONE, notifyData);
	}

	return NPERR_NO_ERROR;
}

NPError NPN_GetURL( NPP instance, const char* url, const char* target ) {
	printf( "GetURL %s\n", url );
	if (target && strlen(target)==6 && memcmp("_blank",target,6)==0) {
		//system_launch_url(url);
		return NPERR_NO_ERROR;
	}
	return NPERR_NO_ERROR;
}

NPError NPN_PostURLNotify( NPP instance, const char* url, const char* target, uint32_t len, const char* buf, NPBool file, void* notifyData) {
	printf( "PostURLNotify (url) %s (target) %s (buf) %s[%d] (file) %i (nd) %p\n", url, target,  buf, len, file, notifyData);
	//url_process(instance,url,buf,len,notifyData);
	return NPERR_NO_ERROR;
}

NPError NPN_PostURL( NPP instance, const char* url, const char* target, uint32_t len, const char* buf, NPBool file ) {
	printf( "PostURL" );
	return NPERR_NO_ERROR;
}

NPError NPN_RequestRead(NPStream* stream, NPByteRange* range_list) {
	printf("[D] NPN_RequestRead\n");
        return NPERR_NO_ERROR;
}

NPError NewStream( NPP instance, NPMIMEType type, const char* target, NPStream** stream ) {
	printf( "NewStream\n" );
	return NPERR_NO_ERROR;
}

int32_t Write( NPP instance, NPStream* stream, int32_t len, void* buffer ) {
	printf( "Write\n" );
	return 0;
}

NPError DestroyStream( NPP instance, NPStream* stream, NPReason reason ) {
	printf( "DestroyStream\n" );
	return NPERR_NO_ERROR;
}

void _InvalidateRect( NPP instance, NPRect *invalidRect ) {
	printf( "InvalidateRect\n" );
	
}

void InvalidateRegion( NPP instance, NPRegion invalidRegion ) {
	printf( "InvalidateRegion\n" );
}

void ForceRedraw( NPP instance ) {
	printf( "ForceRedraw\n" );
}

void initNPNetscapeFuncs(NPNetscapeFuncs *bFuncs) {
	int i=0;

	for(i=1; i<sizeof(*bFuncs)/sizeof(ssize_t); i++)
	*(((ssize_t*)bFuncs)+i)=i+1000;

	bFuncs->geturl=NPN_GetURL;
	bFuncs->posturl=NPN_PostURL;
	bFuncs->requestread=NPN_RequestRead;
	bFuncs->newstream=NewStream;
	bFuncs->write=Write;
	bFuncs->destroystream=DestroyStream;
	bFuncs->status=Status_;
	bFuncs->uagent=NPN_UserAgentProc;
	bFuncs->memalloc=malloc;
	bFuncs->memfree=free;
	bFuncs->memflush=MemFlush;
	bFuncs->reloadplugins=ReloadPlugins;
	bFuncs->getJavaEnv=GetJavaEnv;
	bFuncs->getJavaPeer=GetJavaPeer;
	bFuncs->geturlnotify=NPN_GetURLNotifyProc;
	bFuncs->posturlnotify=NPN_PostURLNotify;
	bFuncs->getvalue=NPN_GetValueProc;
	bFuncs->setvalue=NPN_SetValueProc;
	bFuncs->invalidaterect=_InvalidateRect;
	bFuncs->invalidateregion=InvalidateRegion;
	bFuncs->forceredraw=ForceRedraw;
	bFuncs->getstringidentifier=NPN_GetStringIdentifierProc;
	bFuncs->getstringidentifiers=GetStringIdentifiers;
	bFuncs->getintidentifier=GetIntIdentifier;
	bFuncs->identifierisstring=IdentifierIsString;
	bFuncs->utf8fromidentifier=UTF8FromIdentifier;
	bFuncs->intfromidentifier=IntFromIdentifier;
	bFuncs->createobject=NPN_CreateObjectProc;
	bFuncs->retainobject=NPN_RetainObjectProc;
	bFuncs->releaseobject=NPN_ReleaseObjectProc;
	bFuncs->invoke=NPN_InvokeProc;
	bFuncs->invokeDefault=NPN_InvokeDefaultProc;
	bFuncs->evaluate=Evaluate;
	bFuncs->getproperty=NPN_GetPropertyProc;
	bFuncs->setproperty=NPN_SetPropertyProc;
	bFuncs->removeproperty=NPN_RemovePropertyProc;
	bFuncs->hasproperty=NPN_HasPropertyProc;
	bFuncs->hasmethod=NPN_HasMethodProc;
	bFuncs->releasevariantvalue=NPN_ReleaseVariantValueProc;
	bFuncs->setexception=SetException;

	bFuncs->size= sizeof(bFuncs);
//	bFuncs->version=(NP_VERSION_MAJOR << 8) + NP_VERSION_MINOR;
	bFuncs->version=20;
}

static void destroy(GtkWidget *widget, gpointer data) {
	gtk_main_quit ();
}

int main(int argc, char **argv)
{
	if(argc < 2) {
		printf("[-] Usage: %s <swfuri>\n", argv[0]);
		exit(-1);
	}

	URL = argv[1];

	gtk_init (&argc, &argv);
	main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_usize (main_window, WINDOW_XSIZE, WINDOW_YSIZE);
	g_signal_connect (G_OBJECT (main_window), "destroy", G_CALLBACK (destroy), NULL);
	gtk_widget_realize(main_window);
	//gtk_window_fullscreen((GtkWindow *)main_window);
	gtk_widget_show_all(main_window);

	//gtk_widget_add_events(main_window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
	//g_signal_connect(main_window, "button-press-event", G_CALLBACK(button_handler), &main_window);

	printf("[+] created GTK widget\n");

	flash_plugin_handle = loadFlashPluginSo();

	loadNPEntryPoints(flash_plugin_handle);
	printf("[+] initialized flash plugin entry points\n");

	initNPNetscapeFuncs(&browserFuncs);
	printf("[+] initialized browser functions\n");

	checkError("NP_Initialize", iNP_Initialize(&browserFuncs, &pluginFuncs));

	printPluginEntrypoints(&pluginFuncs);

	printBrowserEntrypoints(&browserFuncs);

	NPWindow *npwin = npwindow_construct(main_window);
	printf("[+] created NPWindow widget\n");

	NPP_t *instancep = (NPP_t *) malloc(sizeof(NPP_t));
	memset(instancep,0,sizeof(sizeof(NPP_t)));
	NPP instance = instancep;

	NPSavedData* saved = (NPSavedData*) malloc(sizeof(NPSavedData));
	memset(saved,0,sizeof(sizeof(NPSavedData)));

	stream = npstream_construct();
	uint16_t stype;
	NPObject object;

	char *xargv[]= {"allowscriptaccess", "name", "quality", "wmode", "allowFullScreen", "width","height","scale"};
	char *xargm[]= {"always", "test", "best", "direct","true","1600","900","exactfit"};


	checkError("NPN_New", pluginFuncs.newp("application/x-shockwave-flash", instance, NP_EMBED, 8, xargv, xargm, 0));
	checkError("NPN_GetValue NPPVpluginScriptableNPObject", pluginFuncs.getvalue(instance, NPPVpluginScriptableNPObject, &object));

	checkError("NPN_SetWindow", pluginFuncs.setwindow(instance, npwin));
	checkError("NPN_NewStream", pluginFuncs.newstream(instance, "application/x-shockwave-flash", stream, 0, &stype));

	FILE *pp;
	char buffer[8192];
	pp = fopen(argv[1],"rb");
	int len;
	while((len=fread(buffer, 1, sizeof(buffer), pp)) != 0) {
		pluginFuncs.writeready(instance, stream);
		pluginFuncs.write(instance, stream, 0, len, buffer);
	}
	fclose(pp);

	checkError("NPN_DestroyStream",pluginFuncs.destroystream(instance, stream, NPRES_DONE));
  
	free(stream);
	gtk_main();

	checkError("NPN_Destroy",pluginFuncs.destroy(instance, &saved));
	checkError("NP_Shutdown", iNP_Shutdown());

	dlclose(flash_plugin_handle);
	return 0;
}
