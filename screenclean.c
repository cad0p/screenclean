#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <ApplicationServices/ApplicationServices.h>

/* As of macOS 10.12.4, brightness set by public IOKit API is
   overridden by CoreDisplay's brightness (to support Night Shift). In
   addition, using CoreDisplay to get brightness supports additional
   display types, e.g. the 2015 iMac's internal display.

   The below functions in CoreDisplay seem to work to adjust the
   "user" brightness (like dragging the slider in System Preferences
   or using function keys).  The symbols below are listed in the .tbd
   file in CoreDisplay.framework so it is at least more "public" than
   a symbol in a private framework, though there are no public headers
   distributed for this framework. */
extern double CoreDisplay_Display_GetUserBrightness(CGDirectDisplayID id)
  __attribute__((weak_import));
extern void CoreDisplay_Display_SetUserBrightness(CGDirectDisplayID id,
												  double brightness)
  __attribute__((weak_import));

/* Some issues with the above CoreDisplay functions include:

   - There's no way to tell if setting the brightness was successful

   - There's no way to tell if a brightness of 1 means that the
	 brightness is actually 1, or if there's no adjustable brightness

   - Brightness changes aren't reflected in System Preferences
	 immediately

   Fixing these means using the private DisplayServices.framework.  Be
   even more careful about these.
*/
extern _Bool DisplayServicesCanChangeBrightness(CGDirectDisplayID id)
  __attribute__((weak_import));
extern void DisplayServicesBrightnessChanged(CGDirectDisplayID id,
											 double brightness)
  __attribute__((weak_import));

const int kMaxDisplays = 16;
const CFStringRef kDisplayBrightness = CFSTR(kIODisplayBrightnessKey);
const char *APP_NAME;

float float_swap(float value){
	union v {
	    float       f;
	    unsigned int    i;
	};

	union v val;

	val.f = value;
	val.i = htonl(val.i);

	return val.f;
}

static void errexit(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s: ", APP_NAME);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  exit(1);
}

static void usage() {
  fprintf(stderr, "usage: %s [-m|-d display] [-v] <brightness>\n", APP_NAME);
  fprintf(stderr, "   or: %s -l [-v]\n", APP_NAME);
  exit(1);
}

static Boolean CFNumberEqualsUInt32(CFNumberRef number, uint32_t uint32) {
  if (number == NULL)
	return (uint32 == 0);

  /* there's no CFNumber type guaranteed to be a uint32, so pick
	 something bigger that's guaranteed not to truncate */
  int64_t int64;
  if (!CFNumberGetValue(number, kCFNumberSInt64Type, &int64))
	return false;

  return (int64 == uint32);
}

/* CGDisplayIOServicePort is deprecated as of 10.9; try to match ourselves */
static io_service_t CGDisplayGetIOServicePort(CGDirectDisplayID dspy) {
  uint32_t vendor = CGDisplayVendorNumber(dspy);
  uint32_t model = CGDisplayModelNumber(dspy); // == product ID
  uint32_t serial = CGDisplaySerialNumber(dspy);

  CFMutableDictionaryRef matching = IOServiceMatching("IODisplayConnect");

  io_iterator_t iter;
  if (IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iter))
	return 0;

  io_service_t service, matching_service = 0;
  while ( (service = IOIteratorNext(iter)) != 0) {
	CFDictionaryRef info = IODisplayCreateInfoDictionary(service, kIODisplayNoProductName);

	CFNumberRef vendorID = CFDictionaryGetValue(info, CFSTR(kDisplayVendorID));
	CFNumberRef productID = CFDictionaryGetValue(info, CFSTR(kDisplayProductID));
	CFNumberRef serialNumber = CFDictionaryGetValue(info, CFSTR(kDisplaySerialNumber));

	if (CFNumberEqualsUInt32(vendorID, vendor) &&
		CFNumberEqualsUInt32(productID, model) &&
		CFNumberEqualsUInt32(serialNumber, serial)) {
	  matching_service = service;

	  CFRelease(info);
	  break;
	}

	CFRelease(info);
  }

  IOObjectRelease(iter);
  return matching_service;
}


int main(int argc, char * const argv[]) {
  APP_NAME = argv[0];
  if (argc == 1)
	usage();

  int verbose = 0;
  CGDirectDisplayID displayToSet = 0;
  enum { ACTION_LIST, ACTION_SET_ALL, ACTION_SET_ONE } action = ACTION_SET_ALL;
  extern char *optarg;
  extern int optind;
  int ch;

  while ( (ch = getopt(argc, argv, "lmvd:")) != -1) {
	switch (ch) {
	case 'l':
	  if (action == ACTION_SET_ONE) usage();
	  action = ACTION_LIST;
	  break;
	case 'v':
	  verbose = 1;
	  break;
	case 'm':
	  if (action != ACTION_SET_ALL) usage();
	  action = ACTION_SET_ONE;
	  displayToSet = CGMainDisplayID();
	  break;
	case 'd':
	  if (action != ACTION_SET_ALL) usage();
	  action = ACTION_SET_ONE;
	  errno = 0;
	  displayToSet = strtoul(optarg, NULL, 0);
	  if (errno == EINVAL || errno == ERANGE)
	errexit("display must be an integer index (0) or a hexadecimal ID (0x4270a80)");
	  break;
	default: usage();
	}
  }

  argc -= optind;
  argv += optind;

  FILE *saveFile;
  char *filename = getenv("HOME");
  strcat(filename, "/.screenclean");
  struct stat st = {0};
  if (stat(filename, &st) == -1) {
  	// the directory is not yet created
  	mkdir(filename, 0755);
  }
  strcat(filename, "/saveFile");
  printf("%s", filename);


  if((saveFile=fopen(filename, "r+")) == NULL) {
	  if((saveFile=fopen(filename, "w+")) == NULL) {
		  errexit("cannot open saveFile");
		  // smadonnate
	  }
  }

  float brightness[kMaxDisplays];
  if (action == ACTION_LIST) {
	if (argc > 0) usage();
  } else {
	if (argc != 1) usage();

	errno = 0;
	printf("%s", argv[0]);
	if (strcmp(argv[0], "on") == 0) {
		for (int i = 0; i < kMaxDisplays; ++i) {
			brightness[i] = 1;
		}
	}
	else if (strcmp(argv[0], "off") == 0) {
		if (fread(brightness,
			   sizeof(float),
			   kMaxDisplays,
			   saveFile) != kMaxDisplays) {
			if (feof(saveFile)) {
				printf("%f", brightness[0]);
				errexit("premature EOF while reading saveFile");
			} else {
				errexit("error reading saveFile");
			}
		}
	}
	else {
		for (int i = 0; i < kMaxDisplays; ++i) {
			brightness[i] = strtof(argv[0], NULL);
		}
	}
	if (errno == ERANGE)
		usage();

	for (int i = 0; i < kMaxDisplays; ++i) {
		printf("Brightness: %f\n", brightness[i]);
		if (brightness[i] < 0 || brightness[i] > 1)
		  errexit("brightness must be between 0 and 1");
	}

  }

  CGDirectDisplayID display[kMaxDisplays];
  float oldBrightness[kMaxDisplays];
  for (int i = 0; i < kMaxDisplays; ++i) {
  	oldBrightness[i] = 0;
  }
  CGDisplayCount numDisplays;
  CGDisplayErr err;
  err = CGGetOnlineDisplayList(kMaxDisplays, display, &numDisplays);
  if (err != CGDisplayNoErr)
	errexit("cannot get list of displays (error %d)\n", err);

  CFWriteStreamRef stdoutStream = NULL;
  if (verbose) {
	CFURLRef devStdout =
	  CFURLCreateWithFileSystemPath(NULL, CFSTR("/dev/stdout"),
					kCFURLPOSIXPathStyle, false);
	stdoutStream = CFWriteStreamCreateWithFile(NULL, devStdout);
	if (stdoutStream == NULL)
	  errexit("cannot create CFWriteStream for /dev/stdout");
	if (!CFWriteStreamOpen(stdoutStream))
	  errexit("cannot open CFWriteStream for /dev/stdout");
  }

  for (CGDisplayCount i = 0; i < numDisplays; ++i) {
  	CGDirectDisplayID dspy = display[i];
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(dspy);
	if (mode == NULL)
	  continue;

	if (action == ACTION_LIST) {
	  printf("display %d: ", i);
	  if (CGDisplayIsMain(dspy))
	printf("main, ");
	  printf("%sactive, %s, %sline, %s%s",
			 CGDisplayIsActive(dspy) ? "" : "in",
			 CGDisplayIsAsleep(dspy) ? "asleep" : "awake",
			 CGDisplayIsOnline(dspy) ? "on" : "off",
			 CGDisplayIsBuiltin(dspy) ? "built-in" : "external",
			 CGDisplayIsStereo(dspy) ? ", stereo" : "");
	  printf(", ID 0x%x\n", (unsigned int)dspy);
	  if (verbose) {
		CGRect bounds = CGDisplayBounds(dspy);
		printf("\tresolution %.0f x %.0f pt",
			   bounds.size.width, bounds.size.height);
		printf(" (%zu x %zu px)",
			   CGDisplayModeGetPixelWidth(mode),
			   CGDisplayModeGetPixelHeight(mode));
		double refreshRate = CGDisplayModeGetRefreshRate(mode);
		if (refreshRate != 0)
		  printf(" @ %.1f Hz", refreshRate);
		printf(", origin (%.0f, %.0f)\n",
			   bounds.origin.x, bounds.origin.y);
		CGSize size = CGDisplayScreenSize(dspy);
		printf("\tphysical size %.0f x %.0f mm",
			   size.width, size.height);
		double rotation = CGDisplayRotation(dspy);
		if (rotation)
		  printf(", rotated %.0f°", rotation);
		printf("\n\tIOKit flags 0x%x",
			   CGDisplayModeGetIOFlags(mode));
		printf("; IOKit display mode ID 0x%x\n",
			   CGDisplayModeGetIODisplayModeID(mode));
		if (CGDisplayIsInMirrorSet(dspy)) {
		  CGDirectDisplayID mirrorsDisplay = CGDisplayMirrorsDisplay(dspy);
		  if (mirrorsDisplay == kCGNullDirectDisplay)
			printf("\tmirrored\n");
		  else
			printf("\tmirrors display ID 0x%x\n", mirrorsDisplay);
		}
		printf("\t%susable for desktop GUI%s\n",
			   CGDisplayModeIsUsableForDesktopGUI(mode) ? "" : "not ",
			   CGDisplayUsesOpenGLAcceleration(dspy) ?
			   ", uses OpenGL acceleration" : "");
	  }
	}
	CGDisplayModeRelease(mode);

	io_service_t service = CGDisplayGetIOServicePort(dspy);
	switch (action) {
	case ACTION_SET_ONE:
	  if (displayToSet != dspy && displayToSet != i)
		continue;
	case ACTION_SET_ALL:
	  if (CoreDisplay_Display_SetUserBrightness != NULL) { /* prefer SPI */
		if (DisplayServicesCanChangeBrightness != NULL) {
		  if (!DisplayServicesCanChangeBrightness(dspy)) {
			fprintf(stderr,
					"%s: failed to set brightness of display 0x%x\n",
					APP_NAME, (unsigned int)dspy);
			continue;
		  }
		}
		// set the old brightness
		oldBrightness[i] = CoreDisplay_Display_GetUserBrightness(dspy);
		printf("\noldBrightness: %f\n", oldBrightness[i]);
		CoreDisplay_Display_SetUserBrightness(dspy, brightness[i]);
		if (DisplayServicesBrightnessChanged != NULL) {
		  DisplayServicesBrightnessChanged(dspy, brightness[i]);
		}
	  } else {
		err = IODisplaySetFloatParameter(service, kNilOptions,
										 kDisplayBrightness, brightness[i]);
		if (err != kIOReturnSuccess) {
		  fprintf(stderr,
				  "%s: failed to set brightness of display 0x%x (error %d)\n",
				  APP_NAME, (unsigned int)dspy, err);
		  continue;
		}
	  }
	  if (!verbose) continue;
	case ACTION_LIST:
	  if (CoreDisplay_Display_GetUserBrightness != NULL) { /* prefer SPI */
		if (DisplayServicesCanChangeBrightness != NULL) {
		  if (!DisplayServicesCanChangeBrightness(dspy)) {
			fprintf(stderr,
					"%s: failed to get brightness of display 0x%x\n",
					APP_NAME, (unsigned int)dspy);
			continue;
		  }
		}
		brightness[i] = CoreDisplay_Display_GetUserBrightness(dspy);
	  } else {
		err = IODisplayGetFloatParameter(service, kNilOptions,
										 kDisplayBrightness, &brightness[i]);
		if (err != kIOReturnSuccess) {
		  fprintf(stderr,
				  "%s: failed to get brightness of display 0x%x (error %d)\n",
				  APP_NAME, (unsigned int)dspy, err);
		  continue;
		}
	  }
	  printf("display %d: brightness %f\n", i, brightness[i]);
	}
  }
  for (int i = 0; i < kMaxDisplays; ++i) {
	printf("%f\n", oldBrightness[i]);
  }
  // save old values to be able to restore them later
  if (fwrite(
  	   oldBrightness,
  	   sizeof(float),
  	   kMaxDisplays,
  	   saveFile
  	   ) != kMaxDisplays) {
  	errexit("cannot write to saveFile");
  }

  fclose(saveFile);

  return 0;
}
