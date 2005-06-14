/*
 * xresponse - Interaction latency tester,
 *
 * Written by Ross Burton & Matthew Allum  
 *              <info@openedhand.com> 
 *
 * Copyright (C) 2005 Nokia
 *
 * Licensed under the GPL v2 or greater.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xdamage.h>

/* 
 * defs
 */

#define streq(a,b)      (strcmp(a,b) == 0)

typedef struct Rectangle { int x,y,width,height; } Rectangle;

/*
 * Global variables
 */

FILE      *LogFile = NULL;       /* The file to output the log output too */
int        DamageEventNum;       /* Damage Ext Event ID */
Atom       AtomTimestamp;        /* Atom for getting server time */
int        DamageWaitSecs = 5;   /* Max time to collect damamge */
Rectangle  InterestedDamageRect; /* Damage rect to monitor */
Bool       MouseButtonIsLocked = False; /* For drag code */

int 
handle_xerror(Display *dpy, XErrorEvent *e)
{
  /* Really only here for debugging, for gdb backtrace */
  char msg[255];
  XGetErrorText(dpy, e->error_code, msg, sizeof msg);
  fprintf(stderr, "X error (%#lx): %s (opcode: %i)\n",
	  e->resourceid, msg, e->request_code);

  exit(1);
}

/* for 'dragging' */
void
lock_mouse_button_down(void)
{
  MouseButtonIsLocked = True;
}

void
unlock_mouse_button_down(void)
{
  MouseButtonIsLocked = False;
}

/** 
 * Perform simple logging with timestamp and diff from last log
 */
void
log_action(Time time, int is_stamp, const char *format, ...)
{
  static Time last_time;
  va_list     ap;
  char       *tmp = NULL;
  static int  displayed_header;

  va_start(ap,format);
  vasprintf(&tmp, format, ap);
  va_end(ap);

  if (!displayed_header) 		/* Header */
    {
      fprintf(LogFile, "\n"
	      " Server Time : Diff    : Info\n"
	      "-----------------------------\n");
      displayed_header = 1;
    }

  if (is_stamp)
    {
      fprintf(LogFile, "%s\n", tmp);
    }
  else
    {
      fprintf(LogFile, "%10lums : %5lums : %s",
	      time,
	      (last_time > 0 && time > 0) ? time - last_time : 0,
	      tmp);
    }

  if (time) last_time = time;
  
  if (tmp) free(tmp);
}

/**
 * Get the current timestamp from the X server.
 */
static Time 
get_server_time(Display *dpy) 
{
  XChangeProperty (dpy, DefaultRootWindow (dpy), 
		   AtomTimestamp, AtomTimestamp, 8, 
		   PropModeReplace, "a", 1);
  for (;;) 
    {
      XEvent xevent;

      XMaskEvent (dpy, PropertyChangeMask, &xevent);
      if (xevent.xproperty.atom == AtomTimestamp)
	return xevent.xproperty.time;
    }
}

/**  
 * Get an X event with a timeout ( in secs ). The timeout is
 * updated for the number of secs left.
 */
static Bool
get_xevent_timed(Display        *dpy, 
		 XEvent         *event_return, 
		 struct timeval *tv)      /* in seconds  */
{

  if (tv == NULL || (tv->tv_sec == 0 && tv->tv_usec == 0))
    {
      XNextEvent(dpy, event_return);
      return True;
    }

  XFlush(dpy);

  if (XPending(dpy) == 0) 
    {
      int    fd = ConnectionNumber(dpy);
      fd_set readset;

      FD_ZERO(&readset);
      FD_SET(fd, &readset);

      if (select(fd+1, &readset, NULL, NULL, tv) == 0) 
	return False;
      else 
	{
	  XNextEvent(dpy, event_return);

	  /* *timeout = tv.tv_sec; */ /* XXX Linux only ? */

	  return True;
	}

    } else {
      XNextEvent(dpy, event_return);
      return True;
    }
}

/** 
 * Set up Display connection, required extensions and req other X bits
 */
static Display*
setup_display(char *dpy_name) 
{
  Display *dpy;
  Damage   damage;
  int      unused;

  if ((dpy = XOpenDisplay(dpy_name)) == NULL)
    {
      fprintf (stderr, "Unable to connect to DISPLAY.\n");
      return NULL;
    }

  /* Check the extensions we need are available */

  if (!XTestQueryExtension (dpy, &unused, &unused, &unused, &unused)) {
    fprintf (stderr, "No XTest extension found\n");
    return NULL;
  }

  if (!XDamageQueryExtension (dpy, &DamageEventNum, &unused)) {
    fprintf (stderr, "No DAMAGE extension found\n");
    return NULL;
  }

  /* Set up our interested rect */
  InterestedDamageRect.x      = 0;
  InterestedDamageRect.y      = 0;
  InterestedDamageRect.width  = DisplayWidth(dpy, DefaultScreen(dpy));
  InterestedDamageRect.height = DisplayHeight(dpy, DefaultScreen(dpy));

  XSetErrorHandler(handle_xerror); 

  XSynchronize(dpy, True);

  /* Needed for get_server_time */
  AtomTimestamp = XInternAtom (dpy, "_X_LATENCY_TIMESTAMP", False);  
  XSelectInput(dpy, DefaultRootWindow(dpy), PropertyChangeMask);

  /* XXX Return/global this ? */
  damage = XDamageCreate (dpy, 
			  DefaultRootWindow(dpy), 
			  XDamageReportBoundingBox);
  return dpy;
}


/**
 * Eat all Damage events in the X event queue.
 */
static void 
eat_damage(Display *dpy) 
{
  while (XPending(dpy)) 
    {
      XEvent              xev;
      XDamageNotifyEvent *dev;

      XNextEvent(dpy, &xev);

      if (xev.type == DamageEventNum + XDamageNotify) 
	{
	  dev = (XDamageNotifyEvent*)&xev;
	  XDamageSubtract(dpy, dev->damage, None, None);
	}
    }
}

/** 
 * 'Fakes' a mouse click, returning time sent.
 */
static Time
fake_event(Display *dpy, int x, int y)
{
  Time start;

  XTestFakeMotionEvent(dpy, DefaultScreen(dpy), x, y, CurrentTime);

  /* Eat up any damage caused by above pointer move */
  eat_damage(dpy);

  start = get_server_time(dpy);

  /* Sent click */
  XTestFakeButtonEvent(dpy, Button1, True, CurrentTime);

  if (!MouseButtonIsLocked) 	/* only release if not dragging */
    XTestFakeButtonEvent(dpy, Button1, False, CurrentTime);

  return start;
}

/** 
 * Waits for a damage 'response' to above click
 */
static Bool
wait_response(Display *dpy)
{
  XEvent e;
  struct timeval tv; 
  int    waitsecs, lastsecs;

  tv.tv_sec  = lastsecs = DamageWaitSecs;
  tv.tv_usec = 0;

  while (get_xevent_timed(dpy, &e, &tv))
    {
      if (e.type == DamageEventNum + XDamageNotify) 
	{
	  XDamageNotifyEvent *dev = (XDamageNotifyEvent*)&e;
	  
	  if (dev->area.x >= InterestedDamageRect.x
	      && dev->area.width <= InterestedDamageRect.width
	      && dev->area.y >= InterestedDamageRect.y
	      && dev->area.height <= InterestedDamageRect.height)
	    {
	      log_action(dev->timestamp, 0, "Got damage event %dx%d+%d+%d\n",
			 dev->area.width, dev->area.height, 
			 dev->area.x, dev->area.y);
	    }
	  else waitsecs = lastsecs; /* Reset */
	  
	  XDamageSubtract(dpy, dev->damage, None, None);
	} 
      else 
	{
	  waitsecs = lastsecs; /* Reset */
	  fprintf(stderr, "Got unwanted event type %d\n", e.type);
	}

      fflush(LogFile);

      lastsecs = waitsecs;
    }

  return True;
}

void
usage(char *progname)
{
  fprintf(stderr, "%s: usage, %s <-o|--logfile output> [commands..]\n" 
	          "Commands are any combination/order of;\n"
	          "-c|--click <XxY>                Send click and await damage response\n" 
	          "-d|--drag <XxY,XxY,XxY,XxY..>   Simulate mouse drag and collect damage\n" 

	          "-m|--monitor <WIDTHxHEIGHT+X+Y> Watch area for damage ( default fullscreen )\n"
	          "-w|--wait <seconds>             Max time to wait for damage ( default 5 secs)\n"
	          "-s|--stamp <string>             Write 'string' to log file\n\n"
	          "-i|--inspect                    Just display damage events\n",
	  progname, progname);
  exit(1);
}

int 
main(int argc, char **argv) 
{
  Display *dpy;
  int      cnt, x, y, i = 0;

  if (argc == 1)
    usage(argv[0]);

  if ((dpy = setup_display(getenv("DISPLAY"))) == NULL)
    exit(1);

  if (streq(argv[1],"-o") || streq(argv[1],"--logfile"))
    {
      i++;

      if (++i > argc) usage (argv[0]);

      if ((LogFile = fopen(argv[i], "w")) == NULL)
	fprintf(stderr, "Failed to create logfile '%s'\n", argv[i]);
    }

  if (LogFile == NULL) 
    LogFile = stdout;
  
  while (++i < argc)
    {
      if (streq("-c", argv[i]) || streq("--click", argv[i])) 
	{
	  if (++i>=argc) usage (argv[0]);
	  
	  cnt = sscanf(argv[i], "%ux%u", &x, &y);
	  if (cnt != 2) 
	    {
	      fprintf(stderr, "*** failed to parse '%s'\n", argv[i]);
	      usage(argv[0]);
	    }
	  
	  /* Send the event */
	  log_action(fake_event(dpy, x, y), 0, "Clicked %ix%i\n", x, y);
	  
	  /* .. and wait for the damage response */
	  wait_response(dpy);
	  
	  continue;
	}

      if (streq("-s", argv[i]) || streq("--stamp", argv[i])) 
	{
	  if (++i>=argc) usage (argv[0]);
	  log_action(0, 1, argv[i]);
	  continue;
	}

      
      if (streq("-m", argv[i]) || streq("--monitor", argv[i])) 
	{
	  if (++i>=argc) usage (argv[0]);
	  
	  if ((cnt = sscanf(argv[i], "%ux%u+%u+%u", 
			    &InterestedDamageRect.width,
			    &InterestedDamageRect.height,
			    &InterestedDamageRect.x,
			    &InterestedDamageRect.y)) != 4)
	    {
	      fprintf(stderr, "*** failed to parse '%s'\n", argv[i]);
	      usage(argv[0]);
	    }

	  /*
	  printf("Set monitor rect to %ix%i+%i+%i\n",
		 InterestedDamageRect.x,InterestedDamageRect.y,
		 InterestedDamageRect.width,InterestedDamageRect.height);
	  */

	  continue;
	}

      if (streq("-w", argv[i]) || streq("--wait", argv[i])) 
	{
	  if (++i>=argc) usage (argv[0]);
	  
	  if ((DamageWaitSecs = atoi(argv[i])) < 0)
	    {
	      fprintf(stderr, "*** failed to parse '%s'\n", argv[i]);
	      usage(argv[0]);
	    }
	  /* 
	  log_action(0, "Set event timout to  %isecs\n", DamageWaitSecs);
	  */
	  continue;
	}

      if (streq("-d", argv[i]) || streq("--drag", argv[i])) 
	{
	  char *s = NULL, *p = NULL;
	  
	  if (++i>=argc) usage (argv[0]);

	  s = p = argv[i];



	  while (*p != '\0')
	    {
	      if (*p == ',')
		{
		  lock_mouse_button_down();

		  *p = '\0';

		  cnt = sscanf(s, "%ux%u", &x, &y);
		  if (cnt != 2) 
		    {
		      fprintf(stderr, "*** failed to parse '%s'\n", argv[i]);
		      usage(argv[0]);
		    }
		  
		  /* last passed point make sure button released */
		  if (*(p+1) != ',')
		    unlock_mouse_button_down();

		  /* Send the event */
		  log_action(fake_event(dpy, x, y), 0, 
			     "Dragged to %ix%i\n", x, y);
	  
		  /* .. and wait for the damage response */
		  wait_response(dpy);

		  s = p+1;
		}
	      p++;
	    }
	  

	  continue;
	}

      if (streq("-i", argv[i]) || streq("--inspect", argv[i])) 
	{
	  wait_response(dpy);
	  continue;
	}

      fprintf(stderr, "*** Dont understand  %s\n", argv[i]);
      usage(argv[0]);
    }

  /* Clean Up */

  XCloseDisplay(dpy);
  fclose(LogFile);

  return 0;
}
