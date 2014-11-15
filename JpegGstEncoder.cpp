#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>

// Command line parameters
static char* infile = NULL;
static int width = 0;
static int height = 0;
static char* outfile = NULL;
static int quality = 60;

// Buffer for incoming data (FullHD is max, you can increase it as you need)
static char rawdata[1920*1080];

// Flags to indicate to parent thread that GstreamerThread started and finished
static volatile bool bGstreamerThreadStarted = false;
static volatile bool bGstreamerThreadFinished = false;

static GstElement *appsrc;
static GstElement *appsink;

unsigned int MyGetTickCount()
{
	struct timeval tim;
	gettimeofday(&tim, NULL);
	unsigned int t = ((tim.tv_sec * 1000) + (tim.tv_usec / 1000)) & 0xffffffff;
	return t;
}

void ReadFile(char* name)
{
	FILE* pFile = fopen(name, "rb");
	if(pFile == NULL)
	{
		fprintf(stderr, "Could not open input file. Exiting.\n");
		exit(-1);
	}

	if(fread(rawdata, 1, width * height, pFile) != width * height)
	{
		fclose(pFile);
		fprintf(stderr, "Could not read input file. Exiting.\n");
		exit(-1);
	}
	fclose(pFile);
}

// Bus messages processing, similar to all gstreamer examples
gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
	GMainLoop *loop = (GMainLoop *) data;

	switch (GST_MESSAGE_TYPE (msg))
	{

	case GST_MESSAGE_EOS:
		fprintf(stderr, "End of stream\n");
		g_main_loop_quit(loop);
		break;

	case GST_MESSAGE_ERROR:
	{
		gchar *debug;
		GError *error;

		gst_message_parse_error(msg, &error, &debug);
		g_free(debug);

		g_printerr("Error: %s\n", error->message);
		g_error_free(error);

		g_main_loop_quit(loop);

		break;
	}
	default:
		break;
	}

	return TRUE;
}

// Creates and sets up Gstreamer pipeline for JPEG encoding.
void* GstreamerThread(void* pThreadParam)
{
	GMainLoop *loop;
	GstElement *pipeline, *jpegenc;
	GstBus *bus;
	guint bus_watch_id;

	char* strPipeline = new char[8192];

	/*
	 Pipeline for gstreamer itself is very simple:

	 appsrc -> jpegenc ->appsink

	 However, it won't work without "bufsize" property, it will require buffering in this case.

	The prototype for this pipeline was tested with gst-launch:

	gst-launch-1.0 -v filesrc location=g8.raw blocksize=2073600 ! 'video/x-raw,width=1920,height=1080,format=GRAY8,framerate=0/1' !
	 	 	 jpegenc quality=90 ! filesink location=gstest.jpg

	blocksize must be exactly one raw frame, this will allow not using videoparse and queue in pipeline.
	It means full buffers are fed to jpegenc at once. Pipeline was tested with filesrc and filesink first.
	*/

	pipeline = gst_pipeline_new("mypipeline");
	appsrc = gst_element_factory_make("appsrc", "mysource");
	jpegenc = gst_element_factory_make("jpegenc", "myenc");
	appsink = gst_element_factory_make("appsink", "mysink");

	// Check if all elements were created
	if (!pipeline || !appsrc || !jpegenc || !appsink)
	{
		fprintf(stderr, "Could not gst_element_factory_make, terminating\n");
		bGstreamerThreadStarted = bGstreamerThreadFinished = true;
		return (void*)0xDEAD;
	}

	// appsrc should be linked to jpegenc with these caps otherwise jpegenc does not know size of incoming buffer
	GstCaps *capsappsrc2Jpegenc; // between appsrc and jpegenc
	capsappsrc2Jpegenc = gst_caps_new_simple("video/x-raw", "format",
			G_TYPE_STRING, "GRAY8", "width", G_TYPE_INT, width, "height",
			G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION, 0, 1, NULL);

	// blocksize is important for jpegenc to know how many data to expect from appsrc in a single frame, too
	char szTemp[64];
	sprintf(szTemp, "%d", width * height);
	g_object_set(G_OBJECT (appsrc), "blocksize", szTemp,
			NULL);

	// Jpeg encoding quality
	g_object_set(G_OBJECT (jpegenc), "quality", quality, NULL);

	// Create gstreamer loop
	loop = g_main_loop_new(NULL, FALSE);

	// add a message handler
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
	gst_object_unref(bus);

	// add all elements into pipeline
	gst_bin_add_many(GST_BIN(pipeline), appsrc, jpegenc, appsink, NULL);

	// link elements into pipe: appsrc -> jpegenc -> appsink
	gst_element_link_filtered(appsrc, jpegenc, capsappsrc2Jpegenc);
	gst_element_link(jpegenc, appsink);

	fprintf(stderr, "Setting g_main_loop_run to GST_STATE_PLAYING\n");
	// Start pipeline so it could process incoming data
	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	// Indicate that thread was started
	bGstreamerThreadStarted = true;

	// Loop will run until receiving EOS (end-of-stream), will block here
	g_main_loop_run(loop);

	fprintf(stderr, "g_main_loop_run returned, stopping playback\n");

	// Stop pipeline to be released
	gst_element_set_state(pipeline, GST_STATE_NULL);

	fprintf(stderr, "Deleting pipeline\n");
	// THis will also delete all pipeline elements
	gst_object_unref(GST_OBJECT(pipeline));

	g_source_remove (bus_watch_id);
	g_main_loop_unref (loop);

	// Indicate that thread was finished
	bGstreamerThreadFinished = true;

	return NULL;
}

// Starts GstreamerThread that remains in memory and compresses frames as being fed by user app.
bool StartGstreamer()
{
	unsigned long GtkThreadId;
	pthread_attr_t GtkAttr;

	// Start thread
	int result = pthread_attr_init(&GtkAttr);
	if (result != 0)
	{
		fprintf(stderr, "pthread_attr_init returned error %d\n", result);
		return false;
	}

	void* pParam = NULL;
	result = pthread_create(&GtkThreadId, &GtkAttr,
			GstreamerThread, pParam);
	if (result != 0)
	{
		fprintf(stderr, "pthread_create returned error %d\n", result);
		return false;
	}

	return true;
}

// Puts raw data for encoding into gstreamer. Must put exactly width*height bytes.
void PushBuffer()
{
	GstFlowReturn ret;
	GstBuffer *buffer;

	int size = width*height;
	buffer = gst_buffer_new_allocate(NULL, size, NULL);

	GstMapInfo info;
	gst_buffer_map(buffer, &info, GST_MAP_WRITE);
	unsigned char* buf = info.data;
	memmove(buf, rawdata, size);
	gst_buffer_unmap(buffer, &info);

	ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
}

// Reads compressed jpeg frame. Will block if there is nothing to read out.
char* PullJpeg(int* outlen)
{
	// Will block until sample is ready. In our case "sample" is encoded picture.
	GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));

	if(sample == NULL)
	{
		fprintf(stderr, "gst_app_sink_pull_sample returned null\n");
		return NULL;
	}

	// Actual compressed image is stored inside GstSample.
    GstBuffer* buffer = gst_sample_get_buffer (sample);
	GstMapInfo map;
    gst_buffer_map (buffer, &map, GST_MAP_READ);

    // Allocate appropriate buffer to store compressed image
    char* pRet = new char[map.size];
    // Copy image
    memmove(pRet, map.data, map.size);

    gst_buffer_unmap (buffer, &map);
    gst_sample_unref (sample);

    // Inform caller of image size
    *outlen = map.size;

    return pRet;
}

// Writes image buffer into outfile
void WriteOutFile(char* outfile, char* buf, int len)
{
	FILE* of = fopen(outfile, "wb");
	if(of == NULL)
	{
		fprintf(stderr, "File %s could not be created\n", outfile);
	}
	else
	{
		if(fwrite(buf, 1, len, of) != len)
		{
			fprintf(stderr, "Could not write to file %s\n", outfile);
		}
		fclose(of);
	}
}


int main(int argc, char *argv[])
{

	printf("Testing Gstreamer-1.0 Jpeg encoder.\n");

	/* Check input arguments */
	if (argc < 5 || argc > 6)
	{
		fprintf(stderr,
				"Usage: %s rawfile width height outfile [quality]\n"
						"Rawfile must be one raw frame of GRAY8, outfile will be JPG encoded. 10 outfiles will be created\n",
				argv[0]);
		return -1;
	}
	/* Initialization */
	gst_init(NULL, NULL); // Will abort if GStreamer init error found

	// Read command line arguments
	infile = argv[1];
	width = atoi(argv[2]);
	height = atoi(argv[3]);
	outfile = argv[4];
	quality = 60;
	if (argc == 6)
		quality = atoi(argv[5]);

	// Validate command line arguments
	if (width < 100 || width > 4096 || height < 100 || height > 4096
			|| quality < 10 || quality > 95)
	{
		fprintf(stderr,
				"width and/or height or quality is bad, not running conversion\n");
		return -1;
	}

	// Read raw frame
	ReadFile(infile); // Will exit if file was not read

	// Start conversion thread
	StartGstreamer();

	// Ensure thread is running (or ran and stopped)
	while(bGstreamerThreadStarted == false)
		usleep(10000); // Yield to allow thread to start
	if(bGstreamerThreadFinished == true)
	{
		fprintf(stderr,
				"Gstreamer thread could not start, terminating\n");
		return -1;
	}

	int ticks = MyGetTickCount();

	// Compress raw frame 10 times, adding horizontal stripes to ensure resulting images are different
	for(int i=0; i<10; i++)
	{
		// write stripes into image to see they are really different
		memset(rawdata + (i*20)*width, 0xff, width);
		memset(rawdata + ((i+3)*20)*width, 0x00, width);

		// Push raw buffer into gstreamer
		PushBuffer();

		// Pull compressed buffer from gstreamer
		int len;
		char* buf = PullJpeg(&len);

		// Name output files differently: outfile.jpg -> 0utfile.jpg, 1utfile.jpg, 2utfile.jpg, 3utfile.jpg...
		outfile[0] = i + '0';

		// Write to file
		if(buf != NULL)
			WriteOutFile(outfile, buf, len);
		else
		{
			fprintf(stderr, "Could not read compressed data from gstreamer\n");
		}
		delete[] buf;
	}

	// Get total conversion time
	int ms = MyGetTickCount() - ticks;

	//Tell Gstreamer thread to stop, pushing EOS into gstreamer
	gst_app_src_end_of_stream(GST_APP_SRC(appsrc));

	// Wait until GstreamerThread stops
	for(int i=0; i<100; i++)
	{
		if(bGstreamerThreadFinished == true)
			break;
		usleep(10000); // Yield to allow thread to start
		if(i == 99)
			fprintf(stderr, "GStreamer thread did not finish\n");
	}

	printf("Compressed 10 GRAY8 frames (%d*%d pixels) in %d ms\n", width, height, ms);
	return 0;

}
