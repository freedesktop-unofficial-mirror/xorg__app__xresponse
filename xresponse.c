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

static FILE      *LogFile = NULL;       /* The file to output the log output too */
static int        DamageEventNum;       /* Damage Ext Event ID */
static Atom       AtomTimestamp;        /* Atom for getting server time */
static int        DamageWaitSecs = 5;   /* Max time to collect damamge */
static Rectangle  InterestedDamageRect; /* Damage rect to monitor */
static Time       LastEventTime;      /* When last last event was started */

enum { /* for 'dragging' */
  XR_BUTTON_STATE_NONE,
  XR_BUTTON_STATE_PRESS,
  XR_BUTTON_STATE_RELEASE
};

static int 
handle_xerror(Display *dpy, XErrorEvent *e)
{
  /* Really only here for debugging, for gdb backtrace */
  char msg[255];
  XGetErrorText(dpy, e->error_code, msg, sizeof msg);
  fprintf(stderr, "X error (%#lx): %s (opcode: %i)\n",
	  e->resourceid, msg, e->request_code);

  exit(1);
}


/** 
 * Perform simple logging with timestamp and diff from last log
 */
static void
log_action(Time time, int is_stamp, const char *format, ...)
{
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
	      (LastEventTime > 0 && time > 0) ? time - LastEventTime : 0,
	      tmp);
    }

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

  if (tv == NULL)
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

  /* 'click' mouse */
  XTestFakeButtonEvent(dpy, Button1, True, CurrentTime);
  XTestFakeButtonEvent(dpy, Button1, False, CurrentTime);

  return start;
}

static Time
drag_event(Display *dpy, int x, int y, int button_state)
{
  Time start;

  start = get_server_time(dpy);

  XTestFakeMotionEvent(dpy, DefaultScreen(dpy), x, y, CurrentTime);

  if (button_state == XR_BUTTON_STATE_PRESS)
    {
      eat_damage(dpy); 	/* ignore damage from first drag */
      XTestFakeButtonEvent(dpy, Button1, True, CurrentTime);
    }

  if (button_state == XR_BUTTON_STATE_RELEASE)
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
  struct timeval *timeout = NULL;

  if (DamageWaitSecs)
    {
      tv.tv_sec = DamageWaitSecs;
      tv.tv_usec = 0;
      timeout = &tv;
    }

  while (get_xevent_timed(dpy, &e, timeout))
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
	  
	  XDamageSubtract(dpy, dev->damage, None, None);
	} 
      else 
	{
	  fprintf(stderr, "Got unwanted event type %d\n", e.type);
	}

      fflush(LogFile);
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
	          "-w|--wait <seconds>             Max time to wait for damage, set to 0 to\n"
	          "                                monitor for ever.\n"
	          "                                ( default 5 secs)\n"
	          "-s|--stamp <string>             Write 'string' to log file\n\n"
	          "-i|--inspect                    Just display damage events\n"
	          "-v|--verbose                    Output response to all command line options \n\n",
	  progname, progname);
  exit(1);
}

int 
main(int argc, char **argv) 
{
  Display *dpy;
  int      cnt, x, y, i = 0, verbose = 0;

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
      if (streq(argv[i],"-v") || streq(argv[i],"--verbose"))
        {
	   verbose = 1;
	   continue;
	}

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
	  LastEventTime = fake_event(dpy, x, y);
	  log_action(LastEventTime, 0, "Clicked %ix%i\n", x, y);
	  
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

	  if (verbose)
	      printf("Set monitor rect to %ix%i+%i+%i\n",
		     InterestedDamageRect.width,InterestedDamageRect.height,
		     InterestedDamageRect.x,InterestedDamageRect.y);

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
	  if (verbose)
	    log_action(0, 0, "Set event timout to %isecs\n", DamageWaitSecs);

	  continue;
	}

      if (streq("-d", argv[i]) || streq("--drag", argv[i])) 
	{
	  Time drag_time;
	  char *s = NULL, *p = NULL;
	  int first_drag = 1, button_state = XR_BUTTON_STATE_PRESS;
	  
	  if (++i>=argc) usage (argv[0]);

	  s = p = argv[i];

	  while (1)
	    {
	      if (*p == ',' || *p == '\0')
		{
		  Bool end = False;

		  if (*p == '\0')
		    {
		      if (button_state == XR_BUTTON_STATE_PRESS)
			{
			  fprintf(stderr, 
				  "*** Need at least 2 drag points!\n");
			  usage(argv[0]);
			}

		      /* last passed point so make sure button released */
		      button_state = XR_BUTTON_STATE_RELEASE;
		      end = True;
		    }
		  else *p = '\0';

		  cnt = sscanf(s, "%ux%u", &x, &y);
		  if (cnt != 2) 
		    {
		      fprintf(stderr, "*** failed to parse '%s'\n", argv[i]);
		      usage(argv[0]);
		    }

		  /* Send the event */
		  drag_time = drag_event(dpy, x, y, button_state);
		  if (first_drag)
		    {
		      LastEventTime = drag_time;
		      first_drag = 0;
		    }
		  log_action(drag_time, 0, "Dragged to %ix%i\n", x, y);

		  /* Make sure button state set to none after first point */
		  button_state = XR_BUTTON_STATE_NONE;

		  if (end)
		    break;

		  s = p+1;
		}
	      p++;
	    }

	  /* .. and wait for the damage response */
	  wait_response(dpy);

	  continue;
	}

      if (streq("-i", argv[i]) || streq("--inspect", argv[i])) 
	{
	  if (verbose)
	    log_action(0, 0, "Just displaying damage events until timeout\n");
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
