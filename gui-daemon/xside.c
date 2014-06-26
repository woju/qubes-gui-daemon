/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

/* high level documentation is here:
 * http://wiki.qubes-os.org/trac/wiki/GUIdocs
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <execinfo.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/Xatom.h>
#include <libconfig.h>
#include <qubes-gui-protocol.h>
#include <qubes-xorg-tray-defs.h>
#include "txrx.h"
#include "list.h"
#include "error.h"
#include "png.h"

/* some configuration */

/* default width of forced colorful border */
#define BORDER_WIDTH 2
#define QUBES_CLIPBOARD_FILENAME "/var/run/qubes/qubes-clipboard.bin"
#define QREXEC_CLIENT_PATH "/usr/lib/qubes/qrexec-client"
#define QREXEC_POLICY_PATH "/usr/lib/qubes/qrexec-policy"
#define GUID_CONFIG_FILE "/etc/qubes/guid.conf"
#define GUID_CONFIG_DIR "/etc/qubes"
/* this feature was used to fill icon bg with VM color, later changed to white;
 * discussion: http://wiki.qubes-os.org/trac/ticket/127 */
// #define FILL_TRAY_BG
/* this makes any X11 error fatal (i.e. cause exit(1)). This behavior was the
 * case for a long time before introducing this option, so nothing really have
 * changed  */
#define MAKE_X11_ERRORS_FATAL

// Mod2 excluded as it is Num_Lock
#define SPECIAL_KEYS_MASK (Mod1Mask | Mod3Mask | Mod4Mask | ShiftMask | ControlMask )

// Special window ID meaning "whole screen"
#define FULLSCREEN_WINDOW_ID 0

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

enum clipboard_op {
	CLIPBOARD_COPY,
	CLIPBOARD_PASTE
};

/* per-window data */
struct windowdata {
	unsigned width;
	unsigned height;
	int x;
	int y;
	int is_mapped;
	int is_docked;		/* is it docked tray icon */
	XID remote_winid;	/* window id on VM side */
	Window local_winid;	/* window id on X side */
	struct windowdata *parent;	/* parent window */
	struct windowdata *transient_for;	/* transient_for hint for WM, see http://tronche.com/gui/x/icccm/sec-4.html#WM_TRANSIENT_FOR */
	int override_redirect;	/* see http://tronche.com/gui/x/xlib/window/attributes/override-redirect.html */
	XShmSegmentInfo shminfo;	/* temporary shmid; see shmoverride/README */
	XImage *image;		/* image with window content */
	int image_height;	/* size of window content, not always the same as window in dom0! */
	int image_width;
	int have_queued_configure;	/* have configure request been sent to VM - waiting for confirmation */
	uint32_t flags_set;	/* window flags acked to gui-agent */
};

/* global variables
 * keep them in this struct for readability
 */
struct _global_handles {
	/* local X server handles and attributes */
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	int root_width;		/* size of root window */
	int root_height;
	GC context;		/* context for pixmap operations */
	GC frame_gc;		/* graphic context to paint window frame */
#ifdef FILL_TRAY_BG
	GC tray_gc;		/* graphic context to paint tray background */
#endif
	/* atoms for comunitating with xserver */
	Atom wmDeleteMessage;	/* Atom: WM_DELETE_WINDOW */
	Atom tray_selection;	/* Atom: _NET_SYSTEM_TRAY_SELECTION_S<creen number> */
	Atom tray_opcode;	/* Atom: _NET_SYSTEM_TRAY_MESSAGE_OPCODE */
	Atom xembed_message;	/* Atom: _XEMBED */
	Atom xembed_info;	/* Atom: _XEMBED_INFO */
	Atom wm_state;         /* Atom: _NET_WM_STATE */
	Atom wm_state_fullscreen; /* Atom: _NET_WM_STATE_FULLSCREEN */
	Atom wm_state_demands_attention; /* Atom: _NET_WM_STATE_DEMANDS_ATTENTION */
	Atom frame_extents; /* Atom: _NET_FRAME_EXTENTS */
	/* shared memory handling */
	struct shm_cmd *shmcmd;	/* shared memory with Xorg */
	uint32_t cmd_shmid;		/* shared memory id - received from shmoverride.so through shm.id file */
	int inter_appviewer_lock_fd; /* FD of lock file used to synchronize shared memory access */
	/* Client VM parameters */
	char vmname[32];	/* name of VM */
	int domid;		/* Xen domain id (GUI) */
	int target_domid;		/* Xen domain id (VM) - can differ from domid when GUI is stubdom */
	char *cmdline_color;	/* color of frame */
	char *cmdline_icon;	/* icon hint for WM */
	unsigned long *icon_data; /* loaded icon image, ready for _NEW_WM_ICON property */
	int icon_data_len; /* size of icon_data, in sizeof(*icon_data) units */
	int label_index;	/* label (frame color) hint for WM */
	struct windowdata *screen_window; /* window of whole VM screen */
	/* lists of windows: */
	/*   indexed by remote window id */
	struct genlist *remote2local;
	/*   indexed by local window id */
	struct genlist *wid2windowdata;
	/* counters and other state */
	int clipboard_requested;	/* if clippoard content was requested by dom0 */
	int windows_count;	/* created window count */
	int windows_count_limit;	/* current window limit; ask user what to do when exceeded */
	int windows_count_limit_param; /* initial limit of created windows - after exceed, warning the user */
	struct windowdata *last_input_window;
	/* signal was caught */
	int volatile reload_requested;
	pid_t pulseaudio_pid;
	/* configuration */
	int log_level;		/* log level */
	int startup_timeout;
	int nofork;			   /* do not fork into background - used during guid restart */
	int allow_utf8_titles;	/* allow UTF-8 chars in window title */
	int allow_fullscreen;   /* allow fullscreen windows without decoration */
	int copy_seq_mask;	/* modifiers mask for secure-copy key sequence */
	KeySym copy_seq_key;	/* key for secure-copy key sequence */
	int paste_seq_mask;	/* modifiers mask for secure-paste key sequence */
	KeySym paste_seq_key;	/* key for secure-paste key sequence */
	int qrexec_clipboard;	/* 0: use GUI protocol to fetch/put clipboard, 1: use qrexec */
	int use_kdialog;	/* use kdialog for prompts (default on KDE) or zenity (default on non-KDE) */
	int audio_low_latency; /* set low-latency mode while starting pacat-simple-vchan */
};

typedef struct _global_handles Ghandles;
static Ghandles ghandles;

/* macro used to verify data from VM */
#define VERIFY(x) if (!(x)) { \
		if (ask_whether_verify_failed(g, __STRING(x))) \
			return; \
	}

/* calculate virtual width */
#define XORG_DEFAULT_XINC 8
#define _VIRTUALX(x) ( (((x)+XORG_DEFAULT_XINC-1)/XORG_DEFAULT_XINC)*XORG_DEFAULT_XINC )

/* short macro for beginning of each xevent handling function
 * checks if this window is managed by guid and declares windowdata struct
 * pointer */
#define CHECK_NONMANAGED_WINDOW(g, id) struct windowdata *vm_window; \
	if (!(vm_window=check_nonmanaged_window(g, id))) return

#ifndef min
#define min(x,y) ((x)>(y)?(y):(x))
#endif
#ifndef max
#define max(x,y) ((x)<(y)?(y):(x))
#endif

#define KDIALOG_PATH "/usr/bin/kdialog"
#define ZENITY_PATH "/usr/bin/zenity"

static void inter_appviewer_lock(Ghandles *g, int mode);
static void release_mapped_mfns(Ghandles * g, struct windowdata *vm_window);

static void show_error_message (Ghandles * g, const char *msg)
{
	char message[1024];
	pid_t pid;
	int ret;

	snprintf(message, sizeof message, "Error: VM \"%s\": %s", g->vmname, msg);
	pid = fork();
	switch (pid) {
		case 0:
			if (g->use_kdialog) {
				execlp(KDIALOG_PATH, "kdialog", "--sorry", message, (char*)NULL);
			} else {
				execlp(ZENITY_PATH, "zenity", "--error", "--text", message, (char*)NULL);
			}
			perror("execlp");
			_exit(1);
		case -1:
			perror("fork");
			exit(1);
		default:
			waitpid(pid, &ret, 0);
			ret = WEXITSTATUS(ret);
	}
	switch (ret) {
	case 0:
	case 1:
		return;
	default:
		fprintf(stderr, "Problems executing %s ?\n", g->use_kdialog ? "kdialog" : "zenity");
		exit(1);
	}
}

/* ask user when VM sent invalid message */
static int ask_whether_verify_failed(Ghandles * g, const char *cond)
{
	char text[1024];
	char dontagain_param[128];
	int ret = 1;
	pid_t pid;
	fprintf(stderr, "Verify failed: %s\n", cond);
	/* to be enabled with KDE >= 4.6 in dom0 */
	//#define NEW_KDIALOG
#ifdef NEW_KDIALOG
	snprintf(text, sizeof(text),
			"The domain %s attempted to perform an invalid or suspicious GUI "
			"request. This might be a sign that the domain has been compromised "
			"and is attempting to compromise the GUI daemon (Dom0 domain). In "
			"rare cases, however, it might be possible that a legitimate "
			"application trigger such condition (check the guid logs for more "
			"information). <br/><br/>"
			"Click \"Terminate\" to terminate this domain immediately, or "
			"\"Ignore\" to ignore this condition check and allow the GUI request "
			"to proceed.",
		 g->vmname);
#else
	snprintf(text, sizeof(text),
			"The domain %s attempted to perform an invalid or suspicious GUI "
			"request. This might be a sign that the domain has been compromised "
			"and is attempting to compromise the GUI daemon (Dom0 domain). In "
			"rare cases, however, it might be possible that a legitimate "
			"application trigger such condition (check the guid logs for more "
			"information). <br/><br/>"
			"Do you allow this VM to continue running?",
		 g->vmname);
#endif
	snprintf(dontagain_param, sizeof(dontagain_param), "qubes-quid-%s:%s", g->vmname, cond);

	pid = fork();
	switch (pid) {
		case 0:
			if (g->use_kdialog) {
#ifdef NEW_KDIALOG
				execlp(KDIALOG_PATH, "kdialog", "--dontagain", dontagain_param, "--no-label", "Terminate", "--yes-label", "Ignore", "--warningyesno", text, (char*)NULL);
#else
				execlp(KDIALOG_PATH, "kdialog", "--dontagain", dontagain_param, "--warningyesno", text, (char*)NULL);
#endif
			} else {
				execlp(ZENITY_PATH, "zenity", "--question", "--ok-label", "Terminate", "--cancel-label", "Ignore", "--text", text, (char*)NULL);
			}
			perror("execlp");
			_exit(1);
		case -1:
			perror("fork");
			exit(1);
		default:
			waitpid(pid, &ret, 0);
			ret = WEXITSTATUS(ret);
	}
	if (!g->use_kdialog) {
		// in zenity we use "OK" as "Terminate" to have it default
		// so invert the result
		ret ^= 1;
	}
	switch (ret) {
//	case 2:	/*cancel */
//		break;
	case 0:	/* YES */
		return 0;
	case 1:	/* NO */
		execl("/usr/sbin/xl", "xl", "destroy", g->vmname, (char*)NULL);
		perror("Problems executing xl");
		exit(1);
	default:
		fprintf(stderr, "Problems executing %s ?\n", g->use_kdialog ? "kdialog" : "zenity");
		exit(1);
	}
	/* should never happend */
	return 1;
}

int x11_error_handler(Display * dpy, XErrorEvent * ev)
{
	/* log the error */
	dummy_handler(dpy, ev);
#ifdef MAKE_X11_ERRORS_FATAL
	exit(1);
#endif
	return 0;
}

/* prepare graphic context for painting colorful frame */
static void get_frame_gc(Ghandles * g, const char *name)
{
	XGCValues values;
	XColor fcolor, dummy;
	if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
		unsigned int rgb = strtoul(name, 0, 16);
		fcolor.blue = (rgb & 0xff) * 257;
		rgb >>= 8;
		fcolor.green = (rgb & 0xff) * 257;
		rgb >>= 8;
		fcolor.red = (rgb & 0xff) * 257;
		XAllocColor(g->display,
			    XDefaultColormap(g->display, g->screen),
			    &fcolor);
	} else
		XAllocNamedColor(g->display,
				 XDefaultColormap(g->display, g->screen),
				 name, &fcolor, &dummy);
	values.foreground = fcolor.pixel;
	g->frame_gc =
	    XCreateGC(g->display, g->root_win, GCForeground, &values);
}

#ifdef FILL_TRAY_BG
/* prepare graphic context for tray background */
static void get_tray_gc(Ghandles * g)
{
	XGCValues values;
	values.foreground = WhitePixel(g->display, g->screen);
	g->tray_gc =
	    XCreateGC(g->display, g->root_win, GCForeground, &values);
}
#endif

/* create local window - on VM request.
 * parameters are sanitized already
 */
static Window mkwindow(Ghandles * g, struct windowdata *vm_window)
{
	char *gargv[1] = { NULL };
	Window child_win;
	Window parent;
	XSizeHints my_size_hints;	/* hints for the window manager */
	Atom atom_label;

	my_size_hints.flags = PSize;
	my_size_hints.width = vm_window->width;
	my_size_hints.height = vm_window->height;

	if (vm_window->parent)
		parent = vm_window->parent->local_winid;
	else
		parent = g->root_win;
	// we will set override_redirect later, if needed
	child_win = XCreateSimpleWindow(g->display, parent,
					vm_window->x, vm_window->y,
					vm_window->width,
					vm_window->height, 0,
					BlackPixel(g->display, g->screen),
					WhitePixel(g->display, g->screen));
	/* pass my size hints to the window manager, along with window
	   and icon names */
	(void) XSetStandardProperties(g->display, child_win,
				      "VMapp command", "Pixmap", None,
				      gargv, 0, &my_size_hints);
	(void) XSelectInput(g->display, child_win,
			    ExposureMask | KeyPressMask | KeyReleaseMask |
			    ButtonPressMask | ButtonReleaseMask |
			    PointerMotionMask | EnterWindowMask | LeaveWindowMask |
			    FocusChangeMask | StructureNotifyMask | PropertyChangeMask);
	XSetWMProtocols(g->display, child_win, &g->wmDeleteMessage, 1);
	if (g->icon_data) {
		Atom atom_icon = XInternAtom(g->display, "_NET_WM_ICON", 0);
		XChangeProperty(g->display, child_win, atom_icon, XA_CARDINAL, 32,
				PropModeReplace, (unsigned char *) g->icon_data,
				g->icon_data_len);
		XClassHint class_hint =
		    { g->vmname, g->vmname };
		XSetClassHint(g->display, child_win, &class_hint);
		// perhaps set also icon_pixmap property in WM_HINTS (two Pixmaps -
		// icon and the mask), but hopefully all window managers supports
		// _NET_WM_ICON
	} else if (g->cmdline_icon) {
		XClassHint class_hint =
		    { g->cmdline_icon, g->cmdline_icon };
		XSetClassHint(g->display, child_win, &class_hint);
	}
	// Set '_QUBES_LABEL' property so that Window Manager can read it and draw proper decoration
	atom_label = XInternAtom(g->display, "_QUBES_LABEL", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_CARDINAL,
			8 /* 8 bit is enough */ , PropModeReplace,
			(unsigned char *) &g->label_index, 1);

	// Set '_QUBES_VMNAME' property so that Window Manager can read it and nicely display it
	atom_label = XInternAtom(g->display, "_QUBES_VMNAME", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_STRING,
			8 /* 8 bit is enough */ , PropModeReplace,
			(const unsigned char *) g->vmname,
			strlen(g->vmname));

	if (vm_window->remote_winid == FULLSCREEN_WINDOW_ID) {
		/* whole screen window */
		g->screen_window = vm_window;
	}

	return child_win;
}

/* prepare global variables content:
 * most of them are handles to local Xserver structures */
static void mkghandles(Ghandles * g)
{
	char tray_sel_atom_name[64];
	XWindowAttributes attr;
	g->display = XOpenDisplay(NULL);
	if (!g->display) {
		perror("XOpenDisplay");
		exit(1);
	}
	g->screen = DefaultScreen(g->display);
	g->root_win = RootWindow(g->display, g->screen);
	XGetWindowAttributes(g->display, g->root_win, &attr);
	g->root_width = _VIRTUALX(attr.width);
	g->root_height = attr.height;
	g->context = XCreateGC(g->display, g->root_win, 0, NULL);
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", True);
	g->clipboard_requested = 0;
	snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
		 "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display));
	g->tray_selection =
	    XInternAtom(g->display, tray_sel_atom_name, False);
	g->tray_opcode =
	    XInternAtom(g->display, "_NET_SYSTEM_TRAY_OPCODE", False);
	g->xembed_message = XInternAtom(g->display, "_XEMBED", False);
	g->xembed_info = XInternAtom(g->display, "_XEMBED_INFO", False);
	g->wm_state = XInternAtom(g->display, "_NET_WM_STATE", False);
	g->wm_state_fullscreen = XInternAtom(g->display, "_NET_WM_STATE_FULLSCREEN", False);
	g->wm_state_demands_attention = XInternAtom(g->display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	g->frame_extents = XInternAtom(g->display, "_NET_FRAME_EXTENTS", False);
	/* create graphical contexts */
	get_frame_gc(g, g->cmdline_color ? : "red");
#ifdef FILL_TRAY_BG
	get_tray_gc(g);
#endif
	/* initialize windows limit */
	g->windows_count_limit = g->windows_count_limit_param;
	/* init window lists */
	g->remote2local = list_new();
	g->wid2windowdata = list_new();
	g->screen_window = NULL;
	/* use qrexec for clipboard operations when stubdom GUI is used */
	if (g->domid != g->target_domid)
		g->qrexec_clipboard = 1;
	if (getenv("KDE_SESSION_UID"))
		g->use_kdialog = 1;
	else
		g->use_kdialog = 0;

	g->icon_data = NULL;
	g->icon_data_len = 0;
	if (g->cmdline_icon && g->cmdline_icon[0] == '/') {
		/* in case of error g->icon_data will remain NULL so cmdline_icon will
		 * be used instead (as icon label) */
		g->icon_data = load_png(g->cmdline_icon, &g->icon_data_len);
		if (g->icon_data) {
			fprintf(stderr, "Icon size: %ldx%ld\n", g->icon_data[0], g->icon_data[1]);
		}
	}
	g->inter_appviewer_lock_fd = open("/var/run/qubes/appviewer.lock",
			O_RDWR | O_CREAT, 0666);
	if (g->inter_appviewer_lock_fd < 0) {
		perror("create lock");
		exit(1);
	}
	/* ignore possible errors */
	fchmod(g->inter_appviewer_lock_fd, 0666);
}

/* reload X server parameters, especially after monitor/screen layout change */
void reload(Ghandles * g) {
	XWindowAttributes attr;

	g->screen = DefaultScreen(g->display);
	g->root_win = RootWindow(g->display, g->screen);
	XGetWindowAttributes(g->display, g->root_win, &attr);
	g->root_width = _VIRTUALX(attr.width);
	g->root_height = attr.height;
}

/* find if window (given by id) is managed by this guid */
static struct windowdata *check_nonmanaged_window(Ghandles * g, XID id)
{
	struct genlist *item = list_lookup(g->wid2windowdata, id);
	if (!item) {
		if (g->log_level > 0)
			fprintf(stderr, "cannot lookup 0x%x in wid2windowdata\n",
					(int) id);
		return NULL;
	}
	return item->data;
}

static void save_clipboard_source_vmname(const char *vmname) {
	FILE *file;

	file = fopen(QUBES_CLIPBOARD_FILENAME ".source", "w");
	if (!file) {
		perror("open " QUBES_CLIPBOARD_FILENAME ".source");
		exit(1);
	}
	fwrite(vmname, strlen(vmname), 1, file);
	fclose(file);
}

/* fetch clippboard content from file */
/* lock already taken in is_special_keypress() */
static void get_qubes_clipboard(Ghandles *g, char **data, int *len)
{
	FILE *file;
	*len = 0;
	file = fopen(QUBES_CLIPBOARD_FILENAME, "r");
	if (!file)
		return;
	if (fseek(file, 0, SEEK_END) < 0) {
		show_error_message(g, "secure paste: failed to seek in " QUBES_CLIPBOARD_FILENAME);
		goto close_done;
	}
	*len = ftell(file);
	if (*len < 0) {
		*len = 0;
		show_error_message(g, "secure paste: failed to determine size of "
			QUBES_CLIPBOARD_FILENAME);
		goto close_done;
	}
	if (*len == 0)
		goto close_done;
	*data = malloc(*len);
	if (!*data) {
		perror("malloc");
		exit(1);
	}
	if (fseek(file, 0, SEEK_SET) < 0) {
		free(*data);
		*data = NULL;
		*len = 0;
		show_error_message(g, "secure paste: failed to seek in "
			QUBES_CLIPBOARD_FILENAME);
		goto close_done;
	}
	*len=fread(*data, 1, *len, file);
	if (*len < 0) {
		*len = 0;
		free(*data);
		*data=NULL;
		show_error_message(g, "secure paste: failed to read from "
			QUBES_CLIPBOARD_FILENAME);
		goto close_done;
	}
close_done:
	fclose(file);
	truncate(QUBES_CLIPBOARD_FILENAME, 0);
	save_clipboard_source_vmname("");
}

static int run_clipboard_rpc(Ghandles * g, enum clipboard_op op) {
	char *path_stdin, *path_stdout, *service_call;
	pid_t pid;
	struct rlimit rl;
	int fd;
	char domid_str[16];
	int status;

	switch (op) {
		case CLIPBOARD_COPY:
			path_stdin = "/dev/null";
			path_stdout = QUBES_CLIPBOARD_FILENAME;
			service_call = "DEFAULT:QUBESRPC qubes.ClipboardCopy";
			break;
		case CLIPBOARD_PASTE:
			path_stdin = QUBES_CLIPBOARD_FILENAME;
			path_stdout = "/dev/null";
			service_call = "DEFAULT:QUBESRPC qubes.ClipboardPaste";
			break;
		default:
			/* not reachable */
			return 0;
	}
	switch (pid=fork()) {
		case -1:
			perror("fork");
			exit(1);
		case 0:
			/* in case of error do not use exit(1) in child to not fire
			 * atexit() registered functions; use _exit() instead (which do not
			 * fire that functions) */
			fd = open(path_stdout, O_WRONLY|O_CREAT, 0644);
			if (fd < 0) {
				perror("open");
				_exit(1);
			}
			if (op == CLIPBOARD_COPY) {
				rl.rlim_cur = MAX_CLIPBOARD_SIZE;
				rl.rlim_max = MAX_CLIPBOARD_SIZE;
				setrlimit(RLIMIT_FSIZE, &rl);
				// TODO: place for security filter (via pipe() and another fork+exec)
			}
			dup2(fd, 1);
			close(fd);
			fd = open(path_stdin, O_RDONLY);
			if (fd < 0) {
				perror("open");
				_exit(1);
			}
			dup2(fd, 0);
			close(fd);
			snprintf(domid_str, sizeof(domid_str), "%d", g->target_domid);
			execl(QREXEC_CLIENT_PATH, "qrexec-client", "-d", domid_str, service_call, (char*)NULL);
			perror("execl");
			_exit(1);
		default:
			waitpid(pid, &status, 0);
	}
	return WEXITSTATUS(status) == 0;
}

static int fetch_qubes_clipboard_using_qrexec(Ghandles * g) {
	int ret;

	inter_appviewer_lock(g, 1);
	ret = run_clipboard_rpc(g, CLIPBOARD_COPY);
	if (ret) {
		save_clipboard_source_vmname(g->vmname);
	} else {
		truncate(QUBES_CLIPBOARD_FILENAME, 0);
		save_clipboard_source_vmname("");
	}

	inter_appviewer_lock(g, 0);
	return ret;
}

/* lock already taken in is_special_keypress() */
static int paste_qubes_clipboard_using_qrexec(Ghandles * g) {
	int ret;

	ret = run_clipboard_rpc(g, CLIPBOARD_PASTE);
	if (ret) {
		truncate(QUBES_CLIPBOARD_FILENAME, 0);
		save_clipboard_source_vmname("");
	}

	return ret;
}


/* handle VM message: MSG_CLIPBOARD_DATA
 *  - checks if clipboard data was requested
 *  - store it in file
 */
static void handle_clipboard_data(Ghandles * g, unsigned int untrusted_len)
{
	FILE *file;
	char *untrusted_data;
	size_t untrusted_data_sz;
	if (g->log_level > 0)
		fprintf(stderr, "handle_clipboard_data, len=0x%x\n",
			untrusted_len);
	if (untrusted_len > MAX_CLIPBOARD_SIZE) {
		fprintf(stderr, "clipboard data len 0x%x?\n",
			untrusted_len);
		exit(1);
	}
	/* now sanitized */
	untrusted_data_sz = untrusted_len;
	untrusted_data = malloc(untrusted_data_sz);
	if (!untrusted_data) {
		perror("malloc");
		exit(1);
	}
	read_data(untrusted_data, untrusted_data_sz);
	if (!g->clipboard_requested) {
		free(untrusted_data);
		fprintf(stderr,
			"received clipboard data when not requested\n");
		return;
	}
	inter_appviewer_lock(g, 1);
	file = fopen(QUBES_CLIPBOARD_FILENAME, "w");
	if (!file) {
		show_error_message(g, "secure copy: failed to open file " QUBES_CLIPBOARD_FILENAME);
		goto error;
	}
	if (fwrite(untrusted_data, 1, untrusted_data_sz, file) != untrusted_data_sz) {
		fclose(file);
		show_error_message(g, "secure copy: failed to write to file " QUBES_CLIPBOARD_FILENAME);
		goto error;
	}
	if (fclose(file) < 0) {
		show_error_message(g, "secure copy: failed to close file " QUBES_CLIPBOARD_FILENAME);
		goto error;
	}
	save_clipboard_source_vmname(g->vmname);
error:
	inter_appviewer_lock(g, 0);
	g->clipboard_requested = 0;
	free(untrusted_data);
}

static int evaluate_clipboard_policy(Ghandles * g) {
	int fd, len;
	char source_vm[255];
	int status;
	pid_t pid;

	fd = open(QUBES_CLIPBOARD_FILENAME ".source", O_RDONLY);
	if (fd < 0)
		return 0;

	len = read(fd, source_vm, sizeof(source_vm)-1);
	if (len < 0) {
		perror("read");
		close(fd);
		return 0;
	}
	close(fd);
	if (len == 0) {
		/* empty clipboard */
		return 0;
	}
	source_vm[len] = 0;
	switch(pid=fork()) {
		case -1:
			perror("fork");
			exit(1);
		case 0:
			execl(QREXEC_POLICY_PATH, "qrexec-policy", "--assume-yes-for-ask", "--just-evaluate", source_vm, g->vmname, "qubes.ClipboardPaste", "0", (char*)NULL);
			perror("execl");
			_exit(1);
		default:
			waitpid(pid, &status, 0);
	}
	return WEXITSTATUS(status) == 0;
}

/* check and handle guid-special keys
 * currently only for inter-vm clipboard copy
 */
static int is_special_keypress(Ghandles * g, const XKeyEvent * ev, XID remote_winid)
{
	struct msg_hdr hdr;
	char *data;
	int len;
	if (((int)ev->state & SPECIAL_KEYS_MASK) ==
	    g->copy_seq_mask
	    && ev->keycode == XKeysymToKeycode(g->display,
					       g->copy_seq_key)) {
		if (ev->type != KeyPress)
			return 1;
		if (g->qrexec_clipboard) {
			int ret = fetch_qubes_clipboard_using_qrexec(g);
			if (g->log_level > 0)
				fprintf(stderr, "secure copy: %s\n", ret?"success":"failed");
		} else {
			g->clipboard_requested = 1;
			hdr.type = MSG_CLIPBOARD_REQ;
			hdr.window = remote_winid;
			hdr.untrusted_len = 0;
			if (g->log_level > 0)
				fprintf(stderr, "secure copy\n");
			write_struct(hdr);
		}
		return 1;
	}
	if (((int)ev->state & SPECIAL_KEYS_MASK) ==
	    g->paste_seq_mask
	    && ev->keycode == XKeysymToKeycode(g->display,
					       g->paste_seq_key)) {
		if (ev->type != KeyPress)
			return 1;
		inter_appviewer_lock(g, 1);
		if (!evaluate_clipboard_policy(g)) {
			inter_appviewer_lock(g, 0);
			return 1;
		}
		if (g->qrexec_clipboard) {
			int ret = paste_qubes_clipboard_using_qrexec(g);
			if (g->log_level > 0)
				fprintf(stderr, "secure paste: %s\n", ret?"success":"failed");
		} else {
			hdr.type = MSG_CLIPBOARD_DATA;
			if (g->log_level > 0)
				fprintf(stderr, "secure paste\n");
			get_qubes_clipboard(g, &data, &len);
			if (len > 0) {
				/* MSG_CLIPBOARD_DATA uses the window field to pass the length
				   of the blob */
				hdr.window = len;
				hdr.untrusted_len = len;
				real_write_message((char *) &hdr, sizeof(hdr),
						data, len);
				free(data);
			}
		}
		inter_appviewer_lock(g, 0);

		return 1;
	}
	return 0;
}

/* handle local Xserver event: XKeyEvent
 * send it to relevant window in VM
 */
static void process_xevent_keypress(Ghandles * g, const XKeyEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_keypress k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	g->last_input_window = vm_window;
	if (is_special_keypress(g, ev, vm_window->remote_winid))
		return;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.keycode = ev->keycode;
	hdr.type = MSG_KEYPRESS;
	hdr.window = vm_window->remote_winid;
	write_message(hdr, k);
//      fprintf(stderr, "win 0x%x(0x%x) type=%d keycode=%d\n",
//              (int) ev->window, hdr.window, k.type, k.keycode);
}

// debug routine
#ifdef DEBUG
static void dump_mapped(Ghandles * g)
{
	struct genlist *item = g->wid2windowdata->next;
	for (; item != g->wid2windowdata; item = item->next) {
		struct windowdata *c = item->data;
		if (c->is_mapped) {
			if (g->log_level > 1)
				fprintf(stderr,
					"id 0x%x(0x%x) w=0x%x h=0x%x rx=%d ry=%d ovr=%d\n",
					(int) c->local_winid,
					(int) c->remote_winid, c->width,
					c->height, c->x, c->y,
					c->override_redirect);
		}
	}
}
#endif

/* handle local Xserver event: XButtonEvent
 * same as XKeyEvent - send to relevant window in VM */
static void process_xevent_button(Ghandles * g, const XButtonEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_button k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	g->last_input_window = vm_window;
	k.type = ev->type;

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.button = ev->button;
	hdr.type = MSG_BUTTON;
	hdr.window = vm_window->remote_winid;
	write_message(hdr, k);
	if (g->log_level > 1)
		fprintf(stderr,
			"xside: win 0x%x(0x%x) type=%d button=%d x=%d, y=%d\n",
			(int) ev->window, hdr.window, k.type, k.button,
			k.x, k.y);
	if (vm_window->is_docked && ev->type == ButtonPress) {
		/* Take focus to that icon, to make possible keyboard nagivation
		 * through the menu */
		XSetInputFocus(g->display, vm_window->local_winid, RevertToParent,
				CurrentTime);
	}
}

/* handle local Xserver event: XCloseEvent
 * send to relevant window in VM */
static void process_xevent_close(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	CHECK_NONMANAGED_WINDOW(g, window);
	hdr.type = MSG_CLOSE;
	hdr.window = vm_window->remote_winid;
	hdr.untrusted_len = 0;
	write_struct(hdr);
}

/* send configure request for specified VM window */
static void send_configure(struct windowdata *vm_window, int x, int y, int w,
		    int h)
{
	struct msg_hdr hdr;
	struct msg_configure msg;
	hdr.type = MSG_CONFIGURE;
	hdr.window = vm_window->remote_winid;
	msg.height = h;
	msg.width = w;
	msg.x = x;
	msg.y = y;
	write_message(hdr, msg);
}

/* fix position of docked tray icon;
 * icon position is relative to embedder 0,0 so we must translate it to
 * absolute position */
static int fix_docked_xy(Ghandles * g, struct windowdata *vm_window, const char *caller)
{

	/* docked window is reparented to root_win on vmside */
	Window win;
	int x, y, ret = 0;
	if (XTranslateCoordinates
	    (g->display, vm_window->local_winid, g->root_win,
	     0, 0, &x, &y, &win) == True) {
		/* ignore offscreen coordinates */
		if (x < 0 || y < 0)
			x = y = 0;
		if (vm_window->x != x || vm_window->y != y)
			ret = 1;
		if (g->log_level > 1)
			fprintf(stderr,
				"fix_docked_xy(from %s), calculated xy %d/%d, was "
				"%d/%d\n", caller, x, y, vm_window->x,
				vm_window->y);
		vm_window->x = x;
		vm_window->y = y;
	}
	return ret;
}

/* undo the calculations that fix_docked_xy did, then perform move&resize */
static void moveresize_vm_window(Ghandles * g, struct windowdata *vm_window)
{
	int x = 0, y = 0;
	Window win;
	Atom act_type;
	long *frame_extents; // left, right, top, bottom
	unsigned long nitems, bytesleft;
	int ret, act_fmt;

	if (!vm_window->is_docked) {
		/* we have window content coordinates, but XMoveResizeWindow requires
		 * left top *border* pixel coordinates (if any border is present). */
		ret = XGetWindowProperty(g->display, vm_window->local_winid, g->frame_extents, 0, 4,
				False, XA_CARDINAL, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&frame_extents);
		if (ret == Success && nitems == 4) {
			x = vm_window->x - frame_extents[0];
			y = vm_window->y - frame_extents[2];
			XFree(frame_extents);
		} else {
			/* assume no border */
			x = vm_window->x;
			y = vm_window->y;
		}
	} else
		if (!XTranslateCoordinates(g->display, g->root_win,
				      vm_window->local_winid, vm_window->x,
				      vm_window->y, &x, &y, &win))
			return;
	if (g->log_level > 1)
		fprintf(stderr,
			"XMoveResizeWindow local 0x%x remote 0x%x, xy %d %d (vm_window is %d %d) wh %d %d\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, x, y, vm_window->x,
			vm_window->y, vm_window->width, vm_window->height);
	XMoveResizeWindow(g->display, vm_window->local_winid, x, y,
			  vm_window->width, vm_window->height);
}


/* force window to not hide its frame
 * checks if at least border_width is from every screen edge (and fix if not)
 * Exception: allow window to be entirely off the screen */
static int force_on_screen(Ghandles * g, struct windowdata *vm_window,
		    int border_width, const char *caller)
{
	int do_move = 0, reason = -1;
	int x = vm_window->x, y = vm_window->y, w = vm_window->width, h =
	    vm_window->height;

	if (vm_window->x < border_width
	    && vm_window->x + vm_window->width > 0) {
		vm_window->x = border_width;
		do_move = 1;
		reason = 1;
	}
	if (vm_window->y < border_width
	    && vm_window->y + vm_window->height > 0) {
		vm_window->y = border_width;
		do_move = 1;
		reason = 2;
	}
	if (vm_window->x < g->root_width &&
	    vm_window->x + (int)vm_window->width >
	    g->root_width - border_width) {
		vm_window->width =
		    g->root_width - vm_window->x - border_width;
		do_move = 1;
		reason = 3;
	}
	if (vm_window->y < g->root_height &&
	    vm_window->y + (int)vm_window->height >
	    g->root_height - border_width) {
		vm_window->height =
		    g->root_height - vm_window->y - border_width;
		do_move = 1;
		reason = 4;
	}
	if (do_move)
		if (g->log_level > 0)
			fprintf(stderr,
				"force_on_screen(from %s) returns 1 (reason %d): window 0x%x, xy %d %d, wh %d %d, root %d %d borderwidth %d\n",
				caller, reason,
				(int) vm_window->local_winid, x, y, w, h,
				g->root_width, g->root_height,
				border_width);
	return do_move;
}

/* handle local Xserver event: XConfigureEvent
 * after some checks/fixes send to relevant window in VM */
static void process_xevent_configure(Ghandles * g, const XConfigureEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (g->log_level > 1)
		fprintf(stderr,
			"process_xevent_configure(synth %d) local 0x%x remote 0x%x, %d/%d, was "
			"%d/%d, xy %d/%d was %d/%d\n",
			ev->send_event,
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, ev->width,
			ev->height, vm_window->width, vm_window->height,
			ev->x, ev->y, vm_window->x, vm_window->y);
	/* non-synthetic events are about window position/size relative to the embeding
	 * frame window, wait for the synthetic one (produced by window manager), which
	 * is about window position relative to original window parent.
	 * See http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.5 for details
	 */
	if (!ev->send_event && !vm_window->is_docked)
		return;
	if ((int)vm_window->width == ev->width
	    && (int)vm_window->height == ev->height && vm_window->x == ev->x
	    && vm_window->y == ev->y)
		return;
	vm_window->width = ev->width;
	vm_window->height = ev->height;
	if (!vm_window->is_docked) {
		vm_window->x = ev->x;
		vm_window->y = ev->y;
	} else
		fix_docked_xy(g, vm_window, "process_xevent_configure");

// if AppVM has not unacknowledged previous resize msg, do not send another one
	if (vm_window->have_queued_configure)
		return;
	if (vm_window->remote_winid != FULLSCREEN_WINDOW_ID)
		vm_window->have_queued_configure = 1;
	send_configure(vm_window, vm_window->x, vm_window->y,
		       vm_window->width, vm_window->height);
}

/* handle VM message: MSG_CONFIGURE
 * check if we like new dimensions/position and move relevant window */
static void handle_configure_from_vm(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_configure untrusted_conf;
	int x, y;
	unsigned width, height, override_redirect;
	int conf_changed;

	read_struct(untrusted_conf);
	if (g->log_level > 1)
		fprintf(stderr,
			"handle_configure_from_vm, local 0x%x remote 0x%x, %d/%d, was"
			" %d/%d, ovr=%d, xy %d/%d, was %d/%d\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid,
			untrusted_conf.width, untrusted_conf.height,
			vm_window->width, vm_window->height,
			untrusted_conf.override_redirect, untrusted_conf.x,
			untrusted_conf.y, vm_window->x, vm_window->y);
	/* sanitize start */
	if (untrusted_conf.width > MAX_WINDOW_WIDTH)
		untrusted_conf.width = MAX_WINDOW_WIDTH;
	if (untrusted_conf.height > MAX_WINDOW_HEIGHT)
		untrusted_conf.height = MAX_WINDOW_HEIGHT;
	width = untrusted_conf.width;
	height = untrusted_conf.height;
	VERIFY(width > 0 && height > 0);
	if (untrusted_conf.override_redirect > 0)
		override_redirect = 1;
	else
		override_redirect = 0;
	/* there is no really good limits for x/y, so pass them to Xorg and hope
	 * that everything will be ok... */
	x = untrusted_conf.x;
	y = untrusted_conf.y;
	/* sanitize end */
	if (vm_window->width != width || vm_window->height != height ||
	    vm_window->x != x || vm_window->y != y)
		conf_changed = 1;
	else
		conf_changed = 0;
	vm_window->override_redirect = override_redirect;

	/* We do not allow a docked window to change its size, period. */
	if (vm_window->is_docked) {
		if (conf_changed)
			send_configure(vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
		vm_window->have_queued_configure = 0;
		return;
	}


	if (vm_window->have_queued_configure) {
		if (conf_changed) {
			send_configure(vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
			return;
		} else {
			// same dimensions; this is an ack for our previously sent configure req
			vm_window->have_queued_configure = 0;
		}
	}
	if (!conf_changed)
		return;
	vm_window->width = width;
	vm_window->height = height;
	vm_window->x = x;
	vm_window->y = y;
	if (vm_window->override_redirect)
		// do not let menu window hide its color frame by moving outside of the screen
		// if it is located offscreen, then allow negative x/y
		force_on_screen(g, vm_window, 0,
				"handle_configure_from_vm");
	moveresize_vm_window(g, vm_window);
}

/* handle local Xserver event: EnterNotify, LeaveNotify
 * send it to VM, but alwo we use it to fix docked
 * window position */
static void process_xevent_crossing(Ghandles * g, const XCrossingEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_crossing k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	if (ev->type == EnterNotify) {
		char keys[32];
		XQueryKeymap(g->display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(hdr, keys);
	}
	/* move tray to correct position in VM */
	if (vm_window->is_docked &&
			fix_docked_xy(g, vm_window, "process_xevent_crossing")) {
		send_configure(vm_window, vm_window->x, vm_window->y,
			       vm_window->width, vm_window->height);
	}

	hdr.type = MSG_CROSSING;
	hdr.window = vm_window->remote_winid;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.mode = ev->mode;
	k.detail = ev->detail;
	k.focus = ev->focus;
	write_message(hdr, k);
}

/* handle local Xserver event: XMotionEvent
 * send to relevant window in VM */
static void process_xevent_motion(Ghandles * g, const XMotionEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_motion k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.is_hint = ev->is_hint;
	hdr.type = MSG_MOTION;
	hdr.window = vm_window->remote_winid;
	write_message(hdr, k);
//      fprintf(stderr, "motion in 0x%x", ev->window);
}

/* handle local Xserver event: FocusIn, FocusOut
 * send to relevant window in VM */
static void process_xevent_focus(Ghandles * g, const XFocusChangeEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_focus k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (ev->type == FocusIn) {
		char keys[32];
		XQueryKeymap(g->display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(hdr, keys);
	}
	hdr.type = MSG_FOCUS;
	hdr.window = vm_window->remote_winid;
	k.type = ev->type;
	k.mode = ev->mode;
	k.detail = ev->detail;
	write_message(hdr, k);
}

/* update given fragment of window image
 * can be requested by VM (MSG_SHMIMAGE) and Xserver (XExposeEvent)
 * parameters are not sanitized earlier - we must check it carefully
 * also do not let to cover forced colorful frame (for undecoraded windows)
 */
static void do_shm_update(Ghandles * g, struct windowdata *vm_window,
		   int untrusted_x, int untrusted_y, int untrusted_w,
		   int untrusted_h)
{
	int border_width = BORDER_WIDTH;
	int x = 0, y = 0, w = 0, h = 0;

	/* sanitize start */
	if (untrusted_x < 0 || untrusted_y < 0) {
		if (g->log_level > 1)
			fprintf(stderr,
				"do_shm_update for 0x%x(remote 0x%x), x=%d, y=%d, w=%d, h=%d ?\n",
				(int) vm_window->local_winid,
				(int) vm_window->remote_winid, untrusted_x,
				untrusted_y, untrusted_w, untrusted_h);
		return;
	}
	if (vm_window->image) {
		x = min(untrusted_x, vm_window->image_width);
		y = min(untrusted_y, vm_window->image_height);
		w = min(max(untrusted_w, 0), vm_window->image_width - x);
		h = min(max(untrusted_h, 0), vm_window->image_height - y);
	} else if (g->screen_window) {
		/* update only onscreen window part */
		if (vm_window->x >= g->screen_window->image_width ||
				vm_window->y >= g->screen_window->image_height)
			return;
		if (vm_window->x+untrusted_x < 0)
			untrusted_x = -vm_window->x;
		if (vm_window->y+untrusted_y < 0)
			untrusted_y = -vm_window->y;
		x = min(untrusted_x, g->screen_window->image_width - vm_window->x);
		y = min(untrusted_y, g->screen_window->image_height - vm_window->y);
		w = min(max(untrusted_w, 0), g->screen_window->image_width - vm_window->x - x);
		h = min(max(untrusted_h, 0), g->screen_window->image_height - vm_window->y - y);
	}
	/* else: no image to update, will return after possibly drawing a frame */

	/* sanitize end */

	if (!vm_window->override_redirect) {
		// Window Manager will take care of the frame...
		border_width = 0;
	}

	if (vm_window->is_docked) {
		border_width = 1;
	}

	int do_border = 0;
	int delta, i;
	/* window contains only (forced) frame, so no content to update */
	if ((int)vm_window->width <= border_width * 2
	    || (int)vm_window->height <= border_width * 2) {
		XFillRectangle(g->display, vm_window->local_winid,
			       g->frame_gc, 0, 0,
			       vm_window->width,
			       vm_window->height);
		return;
	}
	if (!vm_window->image && !(g->screen_window && g->screen_window->image))
		return;
	/* force frame to be visible: */
	/*   * left */
	delta = border_width - x;
	if (delta > 0) {
		w -= delta;
		x = border_width;
		do_border = 1;
	}
	/*   * right */
	delta = x + w - (vm_window->width - border_width);
	if (delta > 0) {
		w -= delta;
		do_border = 1;
	}
	/*   * top */
	delta = border_width - y;
	if (delta > 0) {
		h -= delta;
		y = border_width;
		do_border = 1;
	}
	/*   * bottom */
	delta = y + h - (vm_window->height - border_width);
	if (delta > 0) {
		h -= delta;
		do_border = 1;
	}

	/* again check if something left to update */
	if (w <= 0 || h <= 0)
		return;

	if (g->log_level > 1)
		fprintf(stderr,
				"  do_shm_update for 0x%x(remote 0x%x), after border calc: x=%d, y=%d, w=%d, h=%d\n",
				(int) vm_window->local_winid,
				(int) vm_window->remote_winid,
				x, y, w, h);

#ifdef FILL_TRAY_BG
	if (vm_window->is_docked) {
		char *data, *datap;
		size_t data_sz;
		int xp, yp;

		if (!vm_window->image) {
			/* TODO: implement screen_window handling */
			return;
		}
		/* allocate image_width _bits_ for each image line */
		data_sz =
		    (vm_window->image_width / 8 +
		     1) * vm_window->image_height;
		data = datap = calloc(1, data_sz);
		if (!data) {
			perror("malloc(%dx%x -> %zu\n",
				vm_window->image_width, vm_window->image_height, data_sz);
			exit(1);
		}

		/* Create local pixmap, put vmside image to it
		 * then get local image of the copy.
		 * This is needed because XGetPixel does not seem to work
		 * with XShmImage data.
		 *
		 * Always use 0,0 w+x,h+y coordinates to generate proper mask. */
		w = w + x;
		h = h + y;
		if (w > vm_window->image_width)
			w = vm_window->image_width;
		if (h > vm_window->image_height)
			h = vm_window->image_height;
		Pixmap pixmap =
		    XCreatePixmap(g->display, vm_window->local_winid,
				  vm_window->image_width,
				  vm_window->image_height,
				  24);
		XShmPutImage(g->display, pixmap, g->context,
			     vm_window->image, 0, 0, 0, 0,
			     vm_window->image_width,
			     vm_window->image_height, 0);
		XImage *image = XGetImage(g->display, pixmap, 0, 0, w, h,
					  0xFFFFFFFF, ZPixmap);
		/* Use top-left corner pixel color as transparency color */
		unsigned long back = XGetPixel(image, 0, 0);
		/* Generate data for transparency mask Bitmap */
		for (yp = 0; yp < h; yp++) {
			int step = 0;
			for (xp = 0; xp < w; xp++) {
				if (datap - data >= data_sz) {
					fprintf(stderr,
						"Impossible internal error\n");
					exit(1);
				}
				if (XGetPixel(image, xp, yp) != back)
					*datap |= 1 << (step % 8);
				if (step % 8 == 7)
					datap++;
				step++;
			}
			/* ensure that new line will start at new byte */
			if ((step - 1) % 8 != 7)
				datap++;
		}
		Pixmap mask = XCreateBitmapFromData(g->display,
						    vm_window->local_winid,
						    data, w, h);
		/* set trayicon background to white color */
		XFillRectangle(g->display, vm_window->local_winid,
			       g->tray_gc, 0, 0, vm_window->width,
			       vm_window->height);
		/* Paint clipped Image */
		XSetClipMask(g->display, g->context, mask);
		XPutImage(g->display, vm_window->local_winid,
			  g->context, image, 0, 0, 0, 0, w, h);
		/* Remove clipping */
		XSetClipMask(g->display, g->context, None);

		XFreePixmap(g->display, mask);
		XDestroyImage(image);
		XFreePixmap(g->display, pixmap);
		free(data);
		return;
	} else
#endif
	{
		if (vm_window->image) {
			XShmPutImage(g->display, vm_window->local_winid,
					g->context, vm_window->image, x,
					y, x, y, w, h, 0);
		} else {
			XShmPutImage(g->display, vm_window->local_winid,
					g->context, g->screen_window->image, vm_window->x+x,
					vm_window->y+y, x, y, w, h, 0);
		}
	}
	if (!do_border)
		return;
	for (i = 0; i < border_width; i++)
		XDrawRectangle(g->display, vm_window->local_winid,
			       g->frame_gc, i, i,
			       vm_window->width - 1 - 2 * i,
			       vm_window->height - 1 - 2 * i);

}

/* handle local Xserver event: XExposeEvent
 * update relevant part of window using stored image
 */
static void process_xevent_expose(Ghandles * g, const XExposeEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	do_shm_update(g, vm_window, ev->x, ev->y, ev->width, ev->height);
}

/* handle local Xserver event: XMapEvent
 * after some checks, send to relevant window in VM */
static void process_xevent_mapnotify(Ghandles * g, const XMapEvent * ev)
{
	XWindowAttributes attr;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (vm_window->is_mapped)
		return;
	XGetWindowAttributes(g->display, vm_window->local_winid, &attr);
	if (attr.map_state != IsViewable && !vm_window->is_docked) {
		/* Unmap windows that are not visible on vmside.
		 * WM may try to map non-viewable windows ie. when
		 * switching desktops.
		 */
		(void) XUnmapWindow(g->display, vm_window->local_winid);
		if (g->log_level > 1)
			fprintf(stderr, "WM tried to map 0x%x, revert\n",
				(int) vm_window->local_winid);
	} else {
		/* Tray windows shall be visible always */
		struct msg_hdr hdr;
		struct msg_map_info map_info;
		map_info.override_redirect = attr.override_redirect;
		hdr.type = MSG_MAP;
		hdr.window = vm_window->remote_winid;
		write_message(hdr, map_info);
		if (vm_window->is_docked
		    && fix_docked_xy(g, vm_window,
				     "process_xevent_mapnotify"))
			send_configure(vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
	}
}

static inline uint32_t flags_from_atom(Ghandles * g, Atom a) {
	if (a == g->wm_state_fullscreen)
		return WINDOW_FLAG_FULLSCREEN;
	else if (a == g->wm_state_demands_attention)
		return WINDOW_FLAG_DEMANDS_ATTENTION;
	else if (a == XInternAtom(g->display, "_NET_WM_STATE_HIDDEN", False))
		return (1<<2); // TODO: WINDOW_FLAG_MINIMIZE
	else {
		/* ignore unsupported states */
	}
	return 0;
}

/* handle local Xserver event: XPropertyEvent
 * currently only _NET_WM_STATE is examined */
static void process_xevent_propertynotify(Ghandles *g, const XPropertyEvent * ev)
{
	Atom act_type;
	Atom *state_list;
	unsigned long nitems, bytesleft, i;
	int ret, act_fmt;
	uint32_t flags;
	struct msg_hdr hdr;
	struct msg_window_flags msg;

	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (ev->atom == g->wm_state) {
		if (!vm_window->is_mapped)
			return;
		if (ev->state == PropertyNewValue) {
			ret = XGetWindowProperty(g->display, vm_window->local_winid, g->wm_state, 0, 10,
					False, XA_ATOM, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&state_list);
			if (ret == Success && bytesleft > 0) {
			  /* Ensure we read all of the atoms */
			  XFree(state_list);
			  ret = XGetWindowProperty(g->display, vm_window->local_winid, g->wm_state,
			        0, (10 * 4 + bytesleft + 3) / 4, False, XA_ATOM, &act_type, &act_fmt,
			        &nitems, &bytesleft, (unsigned char**)&state_list);
			}
			if (ret != Success) {
				if (g->log_level > 0) {
					fprintf(stderr, "Failed to get 0x%x window state details\n", (int)ev->window);
					return;
				}
			}
			flags = 0;
			for (i = 0; i < nitems; i++) {
				flags |= flags_from_atom(g, state_list[i]);
			}
			XFree(state_list);
		} else { /* PropertyDelete */
			flags = 0;
		}
		if (flags == vm_window->flags_set) {
			/* no change */
			return;
		}
		hdr.type = MSG_WINDOW_FLAGS;
		hdr.window = vm_window->remote_winid;
		msg.flags_set = flags & ~vm_window->flags_set;
		msg.flags_unset = ~flags & vm_window->flags_set;
		write_message(hdr, msg);
		vm_window->flags_set = flags;
	}
}

/* handle local Xserver event: _XEMBED
 * if window isn't mapped already - map it now */
static void process_xevent_xembed(Ghandles * g, const XClientMessageEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (g->log_level > 1)
		fprintf(stderr, "_XEMBED message %ld\n", ev->data.l[1]);
	if (ev->data.l[1] == XEMBED_EMBEDDED_NOTIFY) {
		if (vm_window->is_docked < 2) {
			vm_window->is_docked = 2;
			if (!vm_window->is_mapped)
				XMapWindow(g->display, ev->window);
			/* move tray to correct position in VM */
			if (fix_docked_xy
			    (g, vm_window, "process_xevent_xembed")) {
				send_configure(vm_window, vm_window->x,
					       vm_window->y,
					       vm_window->width,
					       vm_window->height);
			}
		}
	} else if (ev->data.l[1] == XEMBED_FOCUS_IN) {
		struct msg_hdr hdr;
		struct msg_focus k;
		char keys[32];
		XQueryKeymap(g->display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(hdr, keys);
		hdr.type = MSG_FOCUS;
		hdr.window = vm_window->remote_winid;
		k.type = FocusIn;
		k.mode = NotifyNormal;
		k.detail = NotifyNonlinear;
		write_message(hdr, k);
	}

}

/* dispatch local Xserver event */
static void process_xevent(Ghandles * g)
{
	XEvent event_buffer;
	XNextEvent(g->display, &event_buffer);
	switch (event_buffer.type) {
	case KeyPress:
	case KeyRelease:
		process_xevent_keypress(g, (XKeyEvent *) & event_buffer);
		break;
	case ConfigureNotify:
		process_xevent_configure(g, (XConfigureEvent *) &
					 event_buffer);
		break;
	case ButtonPress:
	case ButtonRelease:
		process_xevent_button(g, (XButtonEvent *) & event_buffer);
		break;
	case MotionNotify:
		process_xevent_motion(g, (XMotionEvent *) & event_buffer);
		break;
	case EnterNotify:
	case LeaveNotify:
		process_xevent_crossing(g,
					(XCrossingEvent *) & event_buffer);
		break;
	case FocusIn:
	case FocusOut:
		process_xevent_focus(g,
				     (XFocusChangeEvent *) & event_buffer);
		break;
	case Expose:
		process_xevent_expose(g, (XExposeEvent *) & event_buffer);
		break;
	case MapNotify:
		process_xevent_mapnotify(g, (XMapEvent *) & event_buffer);
		break;
	case PropertyNotify:
		process_xevent_propertynotify(g, (XPropertyEvent *) & event_buffer);
		break;
	case ClientMessage:
//              fprintf(stderr, "xclient, atom=%s\n",
//                      XGetAtomName(g->display,
//                                   event_buffer.xclient.message_type));
		if (event_buffer.xclient.message_type == g->xembed_message) {
			process_xevent_xembed(g, (XClientMessageEvent *) &
					      event_buffer);
		} else if ((Atom)event_buffer.xclient.data.l[0] ==
			   g->wmDeleteMessage) {
			if (g->log_level > 0)
				fprintf(stderr, "close for 0x%x\n",
					(int) event_buffer.xclient.window);
			process_xevent_close(g,
					     event_buffer.xclient.window);
		}
		break;
	default:;
	}
}


/* handle VM message: MSG_SHMIMAGE
 * pass message data to do_shm_update - there input validation will be done */
static void handle_shmimage(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_shmimage untrusted_mx;

	read_struct(untrusted_mx);
	if (!vm_window->is_mapped)
		return;
	if (g->log_level >= 2) {
		fprintf(stderr, "shmimage for 0x%x(remote 0x%x), x: %d, y: %d, w: %d, h: %d\n",
				(int) vm_window->local_winid, (int) vm_window->remote_winid,
				untrusted_mx.x, untrusted_mx.y, untrusted_mx.width,
				untrusted_mx.height);
	}
	/* WARNING: passing raw values, input validation is done inside of
	 * do_shm_update */
	do_shm_update(g, vm_window, untrusted_mx.x, untrusted_mx.y,
		      untrusted_mx.width, untrusted_mx.height);
}

/* ask user when VM creates too many windows */
static void ask_whether_flooding(Ghandles * g)
{
	char text[1024];
	int ret;
	snprintf(text, sizeof(text),
		 "%s %s "
		 "'VMapp \"%s\" has created %d windows; it looks numerous, "
		 "so it may be "
		 "a beginning of a DoS attack. Do you want to continue:'",
		 g->use_kdialog ? KDIALOG_PATH : ZENITY_PATH,
		 g->use_kdialog ? "--yesnocancel" : "--question --text",
		 g->vmname, g->windows_count);
	do {
		ret = system(text);
		ret = WEXITSTATUS(ret);
//              fprintf(stderr, "ret=%d\n", ret);
		switch (ret) {
		case 2:	/*cancel */
			break;
		case 1:	/* NO */
			exit(1);
		case 0:	/*YES */
			g->windows_count_limit += g->windows_count_limit_param;
			break;
		default:
			fprintf(stderr, "Problems executing %s ?\n", g->use_kdialog ? "kdialog" : "zenity");
			exit(1);
		}
	} while (ret == 2);
}

/* handle VM message: MSG_CREATE
 * checks given attributes and create appropriate window in local Xserver
 * (using mkwindow) */
static void handle_create(Ghandles * g, XID window)
{
	struct windowdata *vm_window;
	struct genlist *l;
	struct msg_create untrusted_crt;
	XID parent;

	if (g->windows_count++ > g->windows_count_limit)
		ask_whether_flooding(g);
	vm_window =
	    (struct windowdata *) calloc(1, sizeof(struct windowdata));
	if (!vm_window) {
		perror("malloc(vm_window in handle_create)");
		exit(1);
	}
	/*
	   because of calloc vm_window->image = 0;
	   vm_window->is_mapped = 0;
	   vm_window->local_winid = 0;
	   vm_window->dest = vm_window->src = vm_window->pix = 0;
	 */
	read_struct(untrusted_crt);
	/* sanitize start */
	VERIFY((int) untrusted_crt.width >= 0
	       && (int) untrusted_crt.height >= 0);
	vm_window->width =
	    min((int) untrusted_crt.width, MAX_WINDOW_WIDTH);
	vm_window->height =
	    min((int) untrusted_crt.height, MAX_WINDOW_HEIGHT);
	/* there is no really good limits for x/y, so pass them to Xorg and hope
	 * that everything will be ok... */
	vm_window->x = untrusted_crt.x;
	vm_window->y = untrusted_crt.y;
	if (untrusted_crt.override_redirect)
		vm_window->override_redirect = 1;
	else
		vm_window->override_redirect = 0;
	parent = untrusted_crt.parent;
	/* sanitize end */
	vm_window->remote_winid = window;
	if (!list_insert(g->remote2local, window, vm_window)) {
		fprintf(stderr, "list_insert(g->remote2local failed\n");
		exit(1);
	}
	l = list_lookup(g->remote2local, parent);
	if (l)
		vm_window->parent = l->data;
	else
		vm_window->parent = NULL;
	vm_window->transient_for = NULL;
	vm_window->local_winid = mkwindow(&ghandles, vm_window);
	if (g->log_level > 0)
		fprintf(stderr,
			"Created 0x%x(0x%x) parent 0x%x(0x%x) ovr=%d x/y %d/%d w/h %d/%d\n",
			(int) vm_window->local_winid, (int) window,
			(int) (vm_window->parent ? vm_window->parent->
			       local_winid : 0), (unsigned) parent,
			vm_window->override_redirect,
			vm_window->x, vm_window->y,
			vm_window->width, vm_window->height);
	if (!list_insert
	    (g->wid2windowdata, vm_window->local_winid, vm_window)) {
		fprintf(stderr, "list_insert(g->wid2windowdata failed\n");
		exit(1);
	}

	/* do not allow to hide color frame off the screen */
	if (vm_window->override_redirect
	    && force_on_screen(g, vm_window, 0, "handle_create"))
		moveresize_vm_window(g, vm_window);
}

/* handle VM message: MSG_DESTROY
 * destroy window locally, as requested */
static void handle_destroy(Ghandles * g, struct genlist *l)
{
	struct genlist *l2;
	struct windowdata *vm_window = l->data;
	g->windows_count--;
	if (vm_window == g->last_input_window)
		g->last_input_window = NULL;
	XDestroyWindow(g->display, vm_window->local_winid);
	if (g->log_level > 0)
		fprintf(stderr, " XDestroyWindow 0x%x\n",
			(int) vm_window->local_winid);
	if (vm_window->image)
		release_mapped_mfns(g, vm_window);
	l2 = list_lookup(g->wid2windowdata, vm_window->local_winid);
	list_remove(l);
	list_remove(l2);
	if (vm_window == g->screen_window)
		g->screen_window = NULL;
	free(vm_window);
}

/* validate single UTF-8 character
 * return bytes count of this character, or 0 if the character is invalid */
static int validate_utf8_char(unsigned char *untrusted_c) {
	int tails_count = 0;
	int total_size = 0;
	/* it is safe to access byte pointed by the parameter and the next one
	 * (which can be terminating NULL), but every next byte can access only if
	 * neither of previous bytes was NULL
	 */

	/* According to http://www.ietf.org/rfc/rfc3629.txt:
	 *   UTF8-char   = UTF8-1 / UTF8-2 / UTF8-3 / UTF8-4
	 *   UTF8-1      = %x00-7F
	 *   UTF8-2      = %xC2-DF UTF8-tail
	 *   UTF8-3      = %xE0 %xA0-BF UTF8-tail / %xE1-EC 2( UTF8-tail ) /
	 *                 %xED %x80-9F UTF8-tail / %xEE-EF 2( UTF8-tail )
	 *   UTF8-4      = %xF0 %x90-BF 2( UTF8-tail ) / %xF1-F3 3( UTF8-tail ) /
	 *                 %xF4 %x80-8F 2( UTF8-tail )
	 *   UTF8-tail   = %x80-BF
	 */

	if (*untrusted_c <= 0x7F) {
		return 1;
	} else if (*untrusted_c >= 0xC2 && *untrusted_c <= 0xDF) {
		total_size = 2;
		tails_count = 1;
	} else switch (*untrusted_c) {
		case 0xE0:
			untrusted_c++;
			total_size = 3;
			if (*untrusted_c >= 0xA0 && *untrusted_c <= 0xBF)
				tails_count = 1;
			else
				return 0;
			break;
		case 0xE1: case 0xE2: case 0xE3: case 0xE4:
		case 0xE5: case 0xE6: case 0xE7: case 0xE8:
		case 0xE9: case 0xEA: case 0xEB: case 0xEC:
			/* 0xED */
		case 0xEE:
		case 0xEF:
			total_size = 3;
			tails_count = 2;
			break;
		case 0xED:
			untrusted_c++;
			total_size = 3;
			if (*untrusted_c >= 0x80 && *untrusted_c <= 0x9F)
				tails_count = 1;
			else
				return 0;
			break;
		case 0xF0:
			untrusted_c++;
			total_size = 4;
			if (*untrusted_c >= 0x90 && *untrusted_c <= 0xBF)
				tails_count = 2;
			else
				return 0;
			break;
		case 0xF1:
		case 0xF2:
		case 0xF3:
			total_size = 4;
			tails_count = 3;
			break;
		case 0xF4:
			untrusted_c++;
			if (*untrusted_c >= 0x80 && *untrusted_c <= 0x8F)
				tails_count = 2;
			else
				return 0;
			break;
		default:
			return 0;
	}

	while (tails_count-- > 0) {
		untrusted_c++;
		if (!(*untrusted_c >= 0x80 && *untrusted_c <= 0xBF))
			return 0;
	}
	return total_size;
}

/* replace non-printable characters with '_'
 * given string must be NULL terminated already */
static void sanitize_string_from_vm(unsigned char *untrusted_s, int allow_utf8)
{
	int utf8_ret;
	for (; *untrusted_s; untrusted_s++) {
		// allow only non-control ASCII chars
		if (*untrusted_s >= 0x20 && *untrusted_s <= 0x7E)
			continue;
		if (allow_utf8 && *untrusted_s >= 0x80) {
			utf8_ret = validate_utf8_char(untrusted_s);
			if (utf8_ret > 0) {
				/* loop will do one additional increment */
				untrusted_s += utf8_ret - 1;
				continue;
			}
		}
		*untrusted_s = '_';
	}
}

/* fix menu window parameters: override_redirect and force to not hide its
 * frame */
static void fix_menu(Ghandles * g, struct windowdata *vm_window)
{
	XSetWindowAttributes attr;

	attr.override_redirect = 1;
	XChangeWindowAttributes(g->display, vm_window->local_winid,
				CWOverrideRedirect, &attr);
	vm_window->override_redirect = 1;

	// do not let menu window hide its color frame by moving outside of the screen
	// if it is located offscreen, then allow negative x/y
	if (force_on_screen(g, vm_window, 0, "fix_menu"))
		moveresize_vm_window(g, vm_window);
}

/* handle VM message: MSG_VMNAME
 * remove non-printable characters and pass to X server */
static void handle_wmname(Ghandles * g, struct windowdata *vm_window)
{
	XTextProperty text_prop;
	struct msg_wmname untrusted_msg;
	char buf[sizeof(untrusted_msg.data)];
	char *list[1] = { buf };

	read_struct(untrusted_msg);
	/* sanitize start */
	untrusted_msg.data[sizeof(untrusted_msg.data) - 1] = 0;
	sanitize_string_from_vm((unsigned char *) (untrusted_msg.data),
				g->allow_utf8_titles);
	snprintf(buf, sizeof(buf), "%s", untrusted_msg.data);
	/* sanitize end */
	if (g->log_level > 1)
		fprintf(stderr, "set title for window 0x%x\n",
			(int) vm_window->local_winid);
	Xutf8TextListToTextProperty(g->display, list, 1, XUTF8StringStyle,
				    &text_prop);
	XSetWMName(g->display, vm_window->local_winid, &text_prop);
	XSetWMIconName(g->display, vm_window->local_winid, &text_prop);
	XFree(text_prop.value);
}

/* handle VM message: MSG_WMHINTS
 * Pass hints for window manager to local X server */
static void handle_wmhints(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_window_hints untrusted_msg;
	XSizeHints size_hints;

	memset(&size_hints, 0, sizeof(size_hints));

	read_struct(untrusted_msg);

	/* sanitize start */
	size_hints.flags = 0;
	/* check every value and pass it only when sane */
	if ((untrusted_msg.flags & PMinSize)
	    && untrusted_msg.min_width <= MAX_WINDOW_WIDTH
	    && untrusted_msg.min_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PMinSize;
		size_hints.min_width = untrusted_msg.min_width;
		size_hints.min_height = untrusted_msg.min_height;
	} else
		fprintf(stderr, "invalid PMinSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.min_width, untrusted_msg.min_height);
	if ((untrusted_msg.flags & PMaxSize) && untrusted_msg.max_width > 0
	    && untrusted_msg.max_width <= MAX_WINDOW_WIDTH
	    && untrusted_msg.max_height > 0
	    && untrusted_msg.max_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PMaxSize;
		size_hints.max_width = untrusted_msg.max_width;
		size_hints.max_height = untrusted_msg.max_height;
	} else
		fprintf(stderr, "invalid PMaxSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.max_width, untrusted_msg.max_height);
	if ((untrusted_msg.flags & PResizeInc) && size_hints.width_inc >= 0
	    && size_hints.width_inc < MAX_WINDOW_WIDTH
	    && size_hints.height_inc >= 0
	    && size_hints.height_inc < MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PResizeInc;
		size_hints.width_inc = untrusted_msg.width_inc;
		size_hints.height_inc = untrusted_msg.height_inc;
	} else
		fprintf(stderr, "invalid PResizeInc for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.width_inc, untrusted_msg.height_inc);
	if ((untrusted_msg.flags & PBaseSize) && size_hints.base_width >= 0
	    && size_hints.base_width <= MAX_WINDOW_WIDTH
	    && size_hints.base_height >= 0
	    && size_hints.base_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PBaseSize;
		size_hints.base_width = untrusted_msg.base_width;
		size_hints.base_height = untrusted_msg.base_height;
	} else
		fprintf(stderr, "invalid PBaseSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.base_width,
			untrusted_msg.base_height);
	if (untrusted_msg.flags & PPosition)
		size_hints.flags |= PPosition;
	if (untrusted_msg.flags & USPosition)
		size_hints.flags |= USPosition;
	/* sanitize end */

	if (g->log_level > 1)
		fprintf(stderr,
			"set WM_NORMAL_HINTS for window 0x%x to min=%d/%d, max=%d/%d, base=%d/%d, inc=%d/%d (flags 0x%x)\n",
			(int) vm_window->local_winid, size_hints.min_width,
			size_hints.min_height, size_hints.max_width,
			size_hints.max_height, size_hints.base_width,
			size_hints.base_height, size_hints.width_inc,
			size_hints.height_inc, (int) size_hints.flags);
	XSetWMNormalHints(g->display, vm_window->local_winid, &size_hints);
}

/* handle VM message: MSG_WINDOW_FLAGS
 * Pass window state flags for window manager to local X server */
static void handle_wmflags(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_window_flags untrusted_msg;
	struct msg_window_flags msg;

	read_struct(untrusted_msg);

	/* sanitize start */
	VERIFY((untrusted_msg.flags_set & untrusted_msg.flags_unset) == 0);
	msg.flags_set = untrusted_msg.flags_set & (WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_DEMANDS_ATTENTION | (1<<2));
	msg.flags_unset = untrusted_msg.flags_unset & (WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_DEMANDS_ATTENTION | (1<<2));
	/* sanitize end */

	if (!vm_window->is_mapped) {
		/* for unmapped windows, set property directly; only "set" list is
		 * processed (will override property) */
		Atom state_list[10];
		int i = 0;

		vm_window->flags_set = 0;
		if (msg.flags_set & WINDOW_FLAG_FULLSCREEN) {
			if (g->allow_fullscreen) {
				vm_window->flags_set |= WINDOW_FLAG_FULLSCREEN;
				state_list[i++] = g->wm_state_fullscreen;
			} else {
				/* if fullscreen not allowed, substitute request with maximize */
				state_list[i++] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
				state_list[i++] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
			}
		}
		if (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) {
			vm_window->flags_set |= WINDOW_FLAG_DEMANDS_ATTENTION;
			state_list[i++] = g->wm_state_demands_attention;
		}
		if (i > 0) {
			/* FIXME: error checking? */
			XChangeProperty(g->display, vm_window->local_winid, g->wm_state,
					XA_ATOM, 32, PropModeReplace, (unsigned char*)state_list,
					i);
		} else
			/* just in case */
			XDeleteProperty(g->display, vm_window->local_winid, g->wm_state);
	} else {
		/* for mapped windows, send message to window manager (via root window) */
		XClientMessageEvent ev;
		uint32_t flags_all = msg.flags_set | msg.flags_unset;

		if (!flags_all)
			/* no change requested */
			return;

		// TODO: WINDOW_FLAG_FULLSCREEN and WINDOW_FLAG_MINIMIZE are mutually exclusive

		memset(&ev, 0, sizeof(ev));
		ev.type = ClientMessage;
		ev.display = g->display;
		ev.window = vm_window->local_winid;
		ev.message_type = g->wm_state;
		ev.format = 32;
		ev.data.l[3] = 1; /* source indication: normal application */

		/* ev.data.l[0]: 1 - add/set property, 0 - remove/unset property */
		if (flags_all & WINDOW_FLAG_FULLSCREEN) {
			ev.data.l[0] = (msg.flags_set & WINDOW_FLAG_FULLSCREEN) ? 1 : 0;
			if (g->allow_fullscreen) {
				ev.data.l[1] = g->wm_state_fullscreen;
				ev.data.l[2] = 0;
			} else {
				ev.data.l[1] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
				ev.data.l[2] = XInternAtom(g->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
			}
			XSendEvent(g->display, g->root_win, False,
					(SubstructureNotifyMask|SubstructureRedirectMask),
					(XEvent*) &ev);
		}
		if (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) {
			ev.data.l[0] = (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) ? 1 : 0;
			ev.data.l[1] = g->wm_state_demands_attention;
			ev.data.l[2] = 0;
			XSendEvent(g->display, g->root_win, False,
					(SubstructureNotifyMask|SubstructureRedirectMask),
					(XEvent*) &ev);
		}
		if (msg.flags_set & (1<<2)) { // TODO: WINDOW_FLAG_MINIMIZE
			ev.message_type = XInternAtom(g->display, "WM_CHANGE_STATE", False);
			ev.data.l[0] = IconicState;
			XSendEvent(g->display, g->root_win, False,
					(SubstructureNotifyMask|SubstructureRedirectMask),
					(XEvent*) &ev);
		}
	}
}

/* handle VM message: MSG_MAP
 * Map a window with given parameters */
static void handle_map(Ghandles * g, struct windowdata *vm_window)
{
	struct genlist *trans;
	struct msg_map_info untrusted_txt;

	read_struct(untrusted_txt);
	vm_window->is_mapped = 1;
	if (untrusted_txt.transient_for
	    && (trans =
		list_lookup(g->remote2local,
			    untrusted_txt.transient_for))) {
		struct windowdata *transdata = trans->data;
		vm_window->transient_for = transdata;
		XSetTransientForHint(g->display, vm_window->local_winid,
				     transdata->local_winid);
	} else
		vm_window->transient_for = NULL;
	vm_window->override_redirect = 0;
	if (untrusted_txt.override_redirect)
		fix_menu(g, vm_window);
	(void) XMapWindow(g->display, vm_window->local_winid);
}

/* handle VM message: MSG_DOCK
 * Try to dock window in the tray
 * Rest of XEMBED protocol is catched in VM */
static void handle_dock(Ghandles * g, struct windowdata *vm_window)
{
	Window tray;
	if (g->log_level > 0)
		fprintf(stderr, "docking window 0x%x\n",
			(int) vm_window->local_winid);
	tray = XGetSelectionOwner(g->display, g->tray_selection);
	if (tray != None) {
		long data[2];
		XClientMessageEvent msg;

		data[0] = 0;
		data[1] = 1;
		XChangeProperty(g->display, vm_window->local_winid,
				g->xembed_info, g->xembed_info, 32,
				PropModeReplace, (unsigned char *) data,
				2);

		memset(&msg, 0, sizeof(msg));
		msg.type = ClientMessage;
		msg.window = tray;
		msg.message_type = g->tray_opcode;
		msg.format = 32;
		msg.data.l[0] = CurrentTime;
		msg.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
		msg.data.l[2] = vm_window->local_winid;
		msg.display = g->display;
		XSendEvent(msg.display, msg.window, False, NoEventMask,
			   (XEvent *) & msg);
	}
	vm_window->is_docked = 1;
}

/* Obtain/release inter-vm lock
 * Used for handling shared Xserver memory and clipboard file */
static void inter_appviewer_lock(Ghandles *g, int mode)
{
	int cmd;
	if (mode)
		cmd = LOCK_EX;
	else
		cmd = LOCK_UN;
	if (flock(g->inter_appviewer_lock_fd, cmd) < 0) {
		perror("lock");
		exit(1);
	}
}

/* release shared memory connected with given window */
static void release_mapped_mfns(Ghandles * g, struct windowdata *vm_window)
{
	inter_appviewer_lock(g, 1);
	g->shmcmd->shmid = vm_window->shminfo.shmid;
	XShmDetach(g->display, &vm_window->shminfo);
	XDestroyImage(vm_window->image);
	XSync(g->display, False);
	inter_appviewer_lock(g, 0);
	vm_window->image = NULL;
	shmctl(vm_window->shminfo.shmid, IPC_RMID, 0);
}

/* handle VM message: MSG_MFNDUMP
 * Retrieve memory addresses connected with composition buffer of remote window
 */
static void handle_mfndump(Ghandles * g, struct windowdata *vm_window)
{
	char untrusted_shmcmd_data_from_remote[4096 * SHM_CMD_NUM_PAGES];
	struct shm_cmd *untrusted_shmcmd =
	    (struct shm_cmd *) untrusted_shmcmd_data_from_remote;
	unsigned num_mfn, off;
	static char dummybuf[100];


	if (vm_window->image)
		release_mapped_mfns(g, vm_window);
	read_data(untrusted_shmcmd_data_from_remote,
		  sizeof(struct shm_cmd));

	if (g->log_level > 1)
		fprintf(stderr, "MSG_MFNDUMP for 0x%x(0x%x): %dx%d, num_mfn 0x%x off 0x%x\n",
				(int) vm_window->local_winid, (int) vm_window->remote_winid,
				untrusted_shmcmd->width, untrusted_shmcmd->height,
				untrusted_shmcmd->num_mfn, untrusted_shmcmd->off);
	/* sanitize start */
	VERIFY(untrusted_shmcmd->num_mfn <= (unsigned)MAX_MFN_COUNT);
	num_mfn = untrusted_shmcmd->num_mfn;
	VERIFY((int) untrusted_shmcmd->width >= 0
	       && (int) untrusted_shmcmd->height >= 0);
	VERIFY((int) untrusted_shmcmd->width <= MAX_WINDOW_WIDTH
	       && (int) untrusted_shmcmd->height <= MAX_WINDOW_HEIGHT);
	VERIFY(untrusted_shmcmd->off < 4096);
	off = untrusted_shmcmd->off;
	/* unused for now: VERIFY(untrusted_shmcmd->bpp == 24); */
	/* sanitize end */
	vm_window->image_width = untrusted_shmcmd->width;
	vm_window->image_height = untrusted_shmcmd->height;	/* sanitized above */
	read_data((char *) untrusted_shmcmd->mfns,
		  SIZEOF_SHARED_MFN * num_mfn);
	vm_window->image =
	    XShmCreateImage(g->display,
			    DefaultVisual(g->display, g->screen), 24,
			    ZPixmap, NULL, &vm_window->shminfo,
			    vm_window->image_width,
			    vm_window->image_height);
	if (!vm_window->image) {
		perror("XShmCreateImage");
		exit(1);
	}
	/* the below sanity check must be AFTER XShmCreateImage, it uses vm_window->image */
	if (num_mfn * 4096 <
	    vm_window->image->bytes_per_line * vm_window->image->height +
	    off) {
		fprintf(stderr,
			"handle_mfndump for window 0x%x(remote 0x%x)"
			" got too small num_mfn= 0x%x\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, num_mfn);
		exit(1);
	}
	// temporary shmid; see shmoverride/README
	vm_window->shminfo.shmid =
	    shmget(IPC_PRIVATE, 1, IPC_CREAT | 0700);
	if (vm_window->shminfo.shmid < 0) {
		perror("shmget");
		exit(1);
	}
	/* ensure that _every_ not sanitized field is overrided by some trusted
	 * value */
	untrusted_shmcmd->shmid = vm_window->shminfo.shmid;
	untrusted_shmcmd->domid = g->domid;
	inter_appviewer_lock(g, 1);
	memcpy(g->shmcmd, untrusted_shmcmd_data_from_remote,
           sizeof(struct shm_cmd) + SIZEOF_SHARED_MFN * num_mfn);
    if (SIZEOF_SHARED_MFN * num_mfn + sizeof (struct shm_cmd) < 4096 * SHM_CMD_NUM_PAGES) {
      memset((char*)g->shmcmd->mfns + SIZEOF_SHARED_MFN * num_mfn, 0,
             4096 * SHM_CMD_NUM_PAGES - (SIZEOF_SHARED_MFN * num_mfn + sizeof (struct shm_cmd)));
    }
	vm_window->shminfo.shmaddr = vm_window->image->data = dummybuf;
	vm_window->shminfo.readOnly = True;
	XSync(g->display, False);
	if (!XShmAttach(g->display, &vm_window->shminfo)) {
		fprintf(stderr,
			"XShmAttach failed for window 0x%x(remote 0x%x)\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid);
	}
	XSync(g->display, False);
	g->shmcmd->shmid = g->cmd_shmid;
	inter_appviewer_lock(g, 0);
}

/* VM message dispatcher */
static void handle_message(Ghandles * g)
{
	struct msg_hdr untrusted_hdr;
	uint32_t type;
	XID window = 0;
	struct genlist *l;
	struct windowdata *vm_window = NULL;

	read_struct(untrusted_hdr);
	VERIFY(untrusted_hdr.type > MSG_MIN
	       && untrusted_hdr.type < MSG_MAX);
	/* sanitized msg type */
	type = untrusted_hdr.type;
	if (type == MSG_CLIPBOARD_DATA) {
		/* window field has special meaning here */
		handle_clipboard_data(g, untrusted_hdr.window);
		return;
	}
	l = list_lookup(g->remote2local, untrusted_hdr.window);
	if (type == MSG_CREATE) {
		if (l) {
			fprintf(stderr,
				"CREATE for already existing window id 0x%x?\n",
				untrusted_hdr.window);
			exit(1);
		}
		window = untrusted_hdr.window;
	} else {
		if (!l) {
			fprintf(stderr,
				"msg 0x%x without CREATE for 0x%x\n",
				type, untrusted_hdr.window);
			exit(1);
		}
		vm_window = l->data;
		/* not needed as it is in vm_window struct
		   window = untrusted_hdr.window;
		 */
	}

	switch (type) {
	case MSG_CREATE:
		handle_create(g, window);
		break;
	case MSG_DESTROY:
		handle_destroy(g, l);
		break;
	case MSG_MAP:
		handle_map(g, vm_window);
		break;
	case MSG_UNMAP:
		vm_window->is_mapped = 0;
		(void) XUnmapWindow(g->display, vm_window->local_winid);
		break;
	case MSG_CONFIGURE:
		handle_configure_from_vm(g, vm_window);
		break;
	case MSG_MFNDUMP:
		handle_mfndump(g, vm_window);
		break;
	case MSG_SHMIMAGE:
		handle_shmimage(g, vm_window);
		break;
	case MSG_WMNAME:
		handle_wmname(g, vm_window);
		break;
	case MSG_DOCK:
		handle_dock(g, vm_window);
		break;
	case MSG_WINDOW_HINTS:
		handle_wmhints(g, vm_window);
		break;
	case MSG_WINDOW_FLAGS:
		handle_wmflags(g, vm_window);
		break;
	default:
		fprintf(stderr, "got unknown msg type %d\n", type);
		exit(1);
	}
}

/* signal handler - connected to SIGTERM */
static void dummy_signal_handler(int UNUSED(x))
{
	exit(0);
}

/* signal handler - connected to SIGHUP */
static void sighup_signal_handler(int UNUSED(x))
{
	ghandles.reload_requested = 1;
}

static void print_backtrace(void)
{
	void *array[100];
	size_t size;
	char **strings;
	size_t i;


	if (ghandles.log_level > 1) {
		size = backtrace(array, 100);
		strings = backtrace_symbols(array, size);
		fprintf(stderr, "Obtained %zd stack frames.\n", size);

		for (i = 0; i < size; i++)
			printf("%s\n", strings[i]);

		free(strings);
	}

}

/* release all windows mapped memory */
static void release_all_mapped_mfns(void)
{
	struct genlist *curr;
	if (ghandles.log_level > 1)
		fprintf(stderr, "release_all_mapped_mfns running\n");
	print_backtrace();
	for (curr = ghandles.wid2windowdata->next;
	     curr != ghandles.wid2windowdata; curr = curr->next) {
		struct windowdata *vm_window = curr->data;
		if (vm_window->image)
			/* use og ghandles directly, as no other way get it (atexec cannot
			 * pass argument) */
			release_mapped_mfns(&ghandles, vm_window);
	}
}

/* start pulseaudio Dom0 proxy */
static void exec_pacat(Ghandles * g)
{
	int i, fd, maxfiles;
	pid_t pid;
	char domid_txt[20];
	char logname[80];
	char old_logname[80];
	struct rlimit rl;
	struct stat stat_buf;
	snprintf(domid_txt, sizeof domid_txt, "%d", g->domid);
	snprintf(logname, sizeof logname, "/var/log/qubes/pacat.%s.log",
		 g->vmname);
	snprintf(old_logname, sizeof logname, "/var/log/qubes/pacat.%s.log.old",
		 g->vmname);
	if (stat(logname, &stat_buf) == 0) {
	   if (rename(logname, old_logname) < 0) {
		   perror("Old logfile rename");
	   }
	}
	switch (pid=fork()) {
	case -1:
		perror("fork pacat");
		exit(1);
	case 0:
		maxfiles = getdtablesize();
		if (maxfiles < 0) {
			if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
				maxfiles = rl.rlim_cur;
			else
				maxfiles = 256;
		}
		for (i = 0; i < maxfiles; i++)
			close(i);
		fd = open("/dev/null", O_RDWR); /* stdin */
		dup2(fd, 1); /* stdout */
		umask(0007);
		fd = open(logname, O_WRONLY | O_CREAT | O_TRUNC, 0640); /* stderr */
		umask(0077);
		if (g->audio_low_latency) {
			execl("/usr/bin/pacat-simple-vchan", "pacat-simple-vchan",
					"-l", domid_txt, NULL);
		} else {
			execl("/usr/bin/pacat-simple-vchan", "pacat-simple-vchan",
					domid_txt, NULL);
		}
		perror("execl");
		_exit(1);
	default:
		g->pulseaudio_pid = pid;
	}
}

/* send configuration parameters of X server to VM */
static void send_xconf(Ghandles * g)
{
	struct msg_xconf xconf;
	XWindowAttributes attr;
	XGetWindowAttributes(g->display, g->root_win, &attr);
	xconf.w = _VIRTUALX(attr.width);
	xconf.h = attr.height;
	xconf.depth = attr.depth;
	xconf.mem = xconf.w * xconf.h * 4 / 1024 + 1;
	write_struct(xconf);
}

/* receive from VM and compare protocol version
 * abort if mismatch */
static void get_protocol_version(Ghandles * g)
{
	uint32_t untrusted_version;
	char message[1024];
	uint32_t version_major, version_minor;
	read_struct(untrusted_version);
	version_major = untrusted_version >> 16;
	version_minor = untrusted_version & 0xffff;

	if (version_major == QUBES_GUID_PROTOCOL_VERSION_MAJOR &&
			version_minor <= QUBES_GUID_PROTOCOL_VERSION_MINOR)
		return;
	if (version_major < QUBES_GUID_PROTOCOL_VERSION_MAJOR)
		snprintf(message, sizeof message, "%s %s \""
				"The GUI agent that runs in the VM '%s' implements outdated protocol (%d:%d), and must be updated.\n\n"
				"To start and access the VM or template without GUI virtualization, use the following commands:\n"
				"qvm-start --no-guid vmname\n"
				"sudo xl console vmname\"",
				g->use_kdialog ? KDIALOG_PATH : ZENITY_PATH,
				g->use_kdialog ? "--sorry" : "--error --text ",
				g->vmname, version_major, version_minor);
	else
		snprintf(message, sizeof message, "%s %s \""
				"The Dom0 GUI daemon do not support protocol version %d:%d, requested by the VM '%s'.\n"
				"To update Dom0, use 'qubes-dom0-update' command or do it via qubes-manager\"",
				g->use_kdialog ? KDIALOG_PATH : ZENITY_PATH,
				g->use_kdialog ? "--sorry" : "--error --text ",
				version_major, version_minor, g->vmname);
	system(message);
	exit(1);
}

/* wait until child process connects to VM */
static void wait_for_connection_in_parent(int *pipe_notify)
{
	// inside the parent process
	// wait for daemon to get connection with AppVM
	struct pollfd pipe_pollfd;
	int tries, ret;

	if (ghandles.log_level > 0)
		fprintf(stderr, "Connecting to VM's GUI agent: ");
	close(pipe_notify[1]);	// close the writing end
	pipe_pollfd.fd = pipe_notify[0];
	pipe_pollfd.events = POLLIN;

	for (tries = 0;; tries++) {
		if (ghandles.log_level > 0)
			fprintf(stderr, ".");
		ret = poll(&pipe_pollfd, 1, 1000);
		if (ret < 0) {
			perror("poll");
			exit(1);
		}
		if (ret > 0) {
			if (pipe_pollfd.revents & POLLIN)
				break;
			if (ghandles.log_level > 0)
				fprintf(stderr, "exiting\n");
			exit(1);
		}
		if (tries >= ghandles.startup_timeout) {
			if (ghandles.startup_timeout > 0) {
				if (ghandles.log_level > 0)
					fprintf(stderr, "timeout\n");
				exit(1);
			}
			exit(0);
		}

	}
	if (ghandles.log_level > 0)
		fprintf(stderr, "connected\n");
	exit(0);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: qubes-guid -d domain_id [-c color] [-l label_index] [-i icon name, no suffix, or icon.png path] [-v] [-q] [-a] [-f]\n");
	fprintf(stderr, "       -v  increase log verbosity\n");
	fprintf(stderr, "       -q  decrease log verbosity\n");
	fprintf(stderr, "       -Q  force usage of Qrexec for clipboard operations\n");
	fprintf(stderr, "       -n  do not wait for agent connection\n");
	fprintf(stderr, "       -a  low-latency audio mode\n");
	fprintf(stderr, "       -f  do not fork into background\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Log levels:\n");
	fprintf(stderr, " 0 - only errors\n");
	fprintf(stderr, " 1 - some basic messages (default)\n");
	fprintf(stderr, " 2 - debug\n");
}

static void parse_cmdline(Ghandles * g, int argc, char **argv)
{
	int opt;
	/* defaults */
	g->log_level = 1;
	g->qrexec_clipboard = 0;
	g->nofork = 0;

	while ((opt = getopt(argc, argv, "d:c:l:i:vqQnaf")) != -1) {
		switch (opt) {
		case 'a':
			g->audio_low_latency = 1;
			break;
		case 'd':
			g->domid = atoi(optarg);
			break;
		case 'c':
			g->cmdline_color = optarg;
			break;
		case 'l':
			g->label_index = strtoul(optarg, NULL, 0);
			break;
		case 'i':
			g->cmdline_icon = optarg;
			break;
		case 'q':
			if (g->log_level>0)
				g->log_level--;
			break;
		case 'v':
			g->log_level++;
			break;
		case 'Q':
			g->qrexec_clipboard = 1;
			break;
		case 'n':
			g->startup_timeout = 0;
			break;
		case 'f':
			g->nofork = 1;
			break;
		default:
			usage();
			exit(1);
		}
	}
	if (g->domid<=0) {
		fprintf(stderr, "domid<=0?");
		exit(1);
	}
}

static void load_default_config_values(Ghandles * g)
{

	g->allow_utf8_titles = 0;
	g->copy_seq_mask = ControlMask | ShiftMask;
	g->copy_seq_key = XK_c;
	g->paste_seq_mask = ControlMask | ShiftMask;
	g->paste_seq_key = XK_v;
	g->windows_count_limit_param = 500;
	g->allow_fullscreen = 0;
	g->startup_timeout = 45;
}

// parse string describing key sequence like Ctrl-Alt-c
static void parse_key_sequence(const char *seq, int *mask, KeySym * key)
{
	const char *seqp = seq;
	int found_modifier;

	// ignore null string
	if (seq == NULL)
		return;
	*mask = 0;
	do {
		found_modifier = 1;
		if (strncasecmp(seqp, "Ctrl-", 5) == 0) {
			*mask |= ControlMask;
			seqp += 5;
		} else if (strncasecmp(seqp, "Mod1-", 5) == 0) {
			*mask |= Mod1Mask;
			seqp += 5;
		} else if (strncasecmp(seqp, "Mod3-", 5) == 0) {
			*mask |= Mod3Mask;
			seqp += 5;
		} else if (strncasecmp(seqp, "Mod4-", 5) == 0) {
			*mask |= Mod4Mask;
			seqp += 5;
			/* second name just for convenience */
		} else if (strncasecmp(seqp, "Alt-", 4) == 0) {
			*mask |= Mod1Mask;
			seqp += 4;
		} else if (strncasecmp(seqp, "Shift-", 6) == 0) {
			*mask |= ShiftMask;
			seqp += 6;
		} else
			found_modifier = 0;
	} while (found_modifier);

	*key = XStringToKeysym(seqp);
	if (*key == NoSymbol) {
		fprintf(stderr,
			"Warning: key sequence (%s) is invalid (will be disabled)\n",
			seq);
	}
}

static void parse_vm_config(Ghandles * g, config_setting_t * group)
{
	config_setting_t *setting;

	if ((setting =
	     config_setting_get_member(group, "secure_copy_sequence"))) {
		parse_key_sequence(config_setting_get_string(setting),
				   &g->copy_seq_mask, &g->copy_seq_key);
	}
	if ((setting =
	     config_setting_get_member(group, "secure_paste_sequence"))) {
		parse_key_sequence(config_setting_get_string(setting),
				   &g->paste_seq_mask, &g->paste_seq_key);
	}

	if ((setting =
	     config_setting_get_member(group, "allow_utf8_titles"))) {
		g->allow_utf8_titles = config_setting_get_bool(setting);
	}

	if ((setting =
	     config_setting_get_member(group, "windows_count_limit"))) {
		g->windows_count_limit_param = config_setting_get_int(setting);
	}

	if ((setting =
	     config_setting_get_member(group, "log_level"))) {
		g->log_level = config_setting_get_int(setting);
	}

	if ((setting =
	     config_setting_get_member(group, "allow_fullscreen"))) {
		g->allow_fullscreen = config_setting_get_bool(setting);
	}

	if ((setting =
	     config_setting_get_member(group, "audio_low_latency"))) {
		g->audio_low_latency = config_setting_get_bool(setting);
	}
}

static void parse_config(Ghandles * g)
{
	config_t config;
	config_setting_t *setting;
	char buf[128];

	config_init(&config);
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR > 5)) \
		|| (LIBCONFIG_VER_MAJOR > 1))
	config_set_include_dir(&config, GUID_CONFIG_DIR);
#endif
	if (config_read_file(&config, GUID_CONFIG_FILE) == CONFIG_FALSE) {
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR >= 4)) \
		|| (LIBCONFIG_VER_MAJOR > 1))
		if (config_error_type(&config) == CONFIG_ERR_FILE_IO) {
#else
		if (strcmp(config_error_text(&config), "file I/O error") ==
		    0) {
#endif
			fprintf(stderr,
				"Warning: cannot read config file (%s): %s\n",
				GUID_CONFIG_FILE,
				config_error_text(&config));
		} else {
			fprintf(stderr,
				"Critical: error reading config (%s:%d): %s\n",
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR >= 4)) \
		|| (LIBCONFIG_VER_MAJOR > 1))
				config_error_file(&config),
#else
				GUID_CONFIG_FILE,
#endif
				config_error_line(&config),
				config_error_text(&config));
			exit(1);
		}
	}
	// first load global settings
	if ((setting = config_lookup(&config, "global"))) {
		parse_vm_config(g, setting);
	}
	// then try to load per-VM settings
	snprintf(buf, sizeof(buf), "VM/%s", g->vmname);
	if ((setting = config_lookup(&config, buf))) {
		parse_vm_config(g, setting);
	}
}

/* helper to get a file flag path */
static char *guid_fs_flag(const char *type, int domid)
{
	static char buf[256];
	snprintf(buf, sizeof(buf), "/var/run/qubes/guid-%s.%d",
		 type, domid);
	return buf;
}

static int guid_boot_lock = -1;

/* create guid_running file when connected to VM */
static void set_alive_flag(int domid)
{
	char pid_buf[10];
	int fd = open(guid_fs_flag("running", domid),
		      O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
	snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
	write(fd, pid_buf, strlen(pid_buf));
	close(fd);
	unlink(guid_fs_flag("booting", domid));
	close(guid_boot_lock);

}

/* remove guid_running file at exit */
static void unset_alive_flag(void)
{
	unlink(guid_fs_flag("running", ghandles.domid));
}

static void kill_pacat(void) {
	pid_t pid = ghandles.pulseaudio_pid;
	if (pid > 0) {
		kill(pid, SIGTERM);
	}
}

static void wait_for_pacat(int UNUSED(signum)) {
	int status;

	if (ghandles.pulseaudio_pid > 0) {
		if (waitpid(ghandles.pulseaudio_pid, &status, WNOHANG) > 0) {
			ghandles.pulseaudio_pid = -1;
			if (status != 0 && ghandles.log_level > 0) {
				fprintf(stderr, "pacat exited with %d status\n", status);
			}
		}
	}
}


static void get_boot_lock(int domid)
{
	struct stat st;
	int fd = open(guid_fs_flag("booting", domid),
		      O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		perror("cannot get boot lock ???\n");
		exit(1);
	}
	if (flock(fd, LOCK_EX) < 0) {
		unlink(guid_fs_flag("booting", domid));
		perror("lock");
		exit(1);
	}
	if (!stat(guid_fs_flag("running", domid), &st)) {
		/* guid running, nothing to do */
		unlink(guid_fs_flag("booting", domid));
		exit(0);
	}
	guid_boot_lock = fd;
}

static void cleanup() {
	release_all_mapped_mfns();
	XCloseDisplay(ghandles.display);
	kill_pacat();
	wait_for_pacat(SIGCHLD);
	unset_alive_flag();
	close(ghandles.inter_appviewer_lock_fd);
}

static char** restart_argv;
void restart_guid() {
	cleanup();
	execv("/usr/bin/qubes-guid", restart_argv);
	perror("execv");
}

int main(int argc, char **argv)
{
	int xfd;
	char *vmname;
	FILE *f;
	int childpid;
	int pipe_notify[2];
	char dbg_log[256];
	char dbg_log_old[256];
	int logfd;
	char cmd_tmp[256];
	struct stat stat_buf;

	parse_cmdline(&ghandles, argc, argv);
	get_boot_lock(ghandles.domid);
	/* vmname is required to parse config file */
	vmname = get_vm_name(ghandles.domid, &ghandles.target_domid);
	strncpy(ghandles.vmname, vmname, sizeof(ghandles.vmname));
	ghandles.vmname[sizeof(ghandles.vmname) - 1] = 0;
	free(vmname);
	/* load config file */
	load_default_config_values(&ghandles);
	parse_config(&ghandles);

	if (!ghandles.nofork) {
		// daemonize...
		if (pipe(pipe_notify) < 0) {
			perror("canot create pipe:");
			exit(1);
		}

		childpid = fork();
		if (childpid < 0) {
			fprintf(stderr, "Cannot fork :(\n");
			exit(1);
		} else if (childpid > 0) {
			wait_for_connection_in_parent(pipe_notify);
			exit(0);
		}
		close(pipe_notify[0]);
	}

	// inside the daemonized process...
	f = fopen("/var/run/shm.id", "r");
	if (!f) {
		fprintf(stderr,
			"Missing /var/run/shm.id; run X with preloaded shmoverride\n");
		exit(1);
	}
	fscanf(f, "%d", &ghandles.cmd_shmid);
	fclose(f);
	ghandles.shmcmd = shmat(ghandles.cmd_shmid, NULL, 0);
	if (ghandles.shmcmd == (void *) (-1UL)) {
		fprintf(stderr,
			"Invalid or stale shm id 0x%x in /var/run/shm.id\n",
			ghandles.cmd_shmid);
		exit(1);
	}

	/* prepare argv for possible restarts */
	if (ghandles.nofork) {
		/* "-f" option already given, use the same argv */
		restart_argv = argv;
	} else {
		/* append "-f" option */
		int i;

		restart_argv = malloc((argc+1) * sizeof(char*));
		for (i=0;i<argc;i++)
			restart_argv[i] = argv[i];
		restart_argv[argc-1] = strdup("-f");
		restart_argv[argc] = (char*)NULL;
	}

	if (!ghandles.nofork) {
		/* output redirection only when started as daemon, if "nofork" option
		 * is set as part of guid restart, output is already redirected */
		close(0);
		open("/dev/null", O_RDONLY);
		snprintf(dbg_log, sizeof(dbg_log),
				"/var/log/qubes/guid.%s.log", ghandles.vmname);
		snprintf(dbg_log_old, sizeof(dbg_log_old),
				"/var/log/qubes/guid.%s.log.old", ghandles.vmname);
		if (stat(dbg_log, &stat_buf) == 0) {
			if (rename(dbg_log, dbg_log_old) < 0) {
				perror("Rename old logfile");
			}
		}
		umask(0007);
		logfd = open(dbg_log, O_WRONLY | O_CREAT | O_TRUNC, 0640);
		umask(0077);
		if (logfd < 0) {
			fprintf(stderr,
					"Failed to open log file: %s\n", strerror (errno));
			exit(1);
		}
		dup2(logfd, 1);
		dup2(logfd, 2);
		if (logfd > 2)
			close(logfd);
	}

	chdir("/var/run/qubes");
	errno = 0;
	if (!ghandles.nofork && setsid() < 0) {
		perror("setsid()");
		exit(1);
	}
	mkghandles(&ghandles);
	XSetErrorHandler(x11_error_handler);
	peer_client_init(ghandles.domid, 6000);
	atexit(vchan_close);
	signal(SIGCHLD, wait_for_pacat);
	exec_pacat(&ghandles);
	atexit(kill_pacat);
	/* drop root privileges */
	if (setgid(getgid()) < 0) {
		perror("setgid()");
		exit(1);
	}
	if (setuid(getuid()) < 0) {
		perror("setuid()");
		exit(1);
	}
	set_alive_flag(ghandles.domid);
	atexit(unset_alive_flag);

	if (!ghandles.nofork) {
		write(pipe_notify[1], "Q", 1);	// let the parent know we connected sucessfully
		close (pipe_notify[1]);
	}

	signal(SIGTERM, dummy_signal_handler);
	signal(SIGHUP, sighup_signal_handler);
	atexit(release_all_mapped_mfns);

	xfd = ConnectionNumber(ghandles.display);

	/* provide keyboard map before VM Xserver starts */

	/* cast return value to unsigned, so (unsigned)-1 > sizeof(cmd_tmp) */
	if ((unsigned)snprintf(cmd_tmp, sizeof(cmd_tmp), "/usr/bin/xenstore-write	"
		     "/local/domain/%d/qubes-keyboard \"`/usr/bin/setxkbmap -print`\"",
		     ghandles.domid) < sizeof(cmd_tmp)) {
		/* intentionally ignore return value - don't fail gui-daemon if only
		 * keyboard layout fails */
		system(cmd_tmp);
	}
	vchan_register_at_eof(restart_guid);

	get_protocol_version(&ghandles);
	send_xconf(&ghandles);

	for (;;) {
		int select_fds[2] = { xfd };
		fd_set retset;
		int busy;
		if (ghandles.reload_requested) {
			fprintf(stderr, "reloading X server parameters...\n");
			reload(&ghandles);
			ghandles.reload_requested = 0;
		}
		do {
			busy = 0;
			if (XPending(ghandles.display)) {
				process_xevent(&ghandles);
				busy = 1;
			}
			if (read_ready()) {
				handle_message(&ghandles);
				busy = 1;
			}
		} while (busy);
		wait_for_vchan_or_argfd(1, select_fds, &retset);
	}
	return 0;
}

// vim:ts=4:sw=4:noet:
