#pragma once

#include <jni.h>

namespace callbacks
{
	/*!
	Sets the JNIEnv which the callbacks should use. Must be set for any Java
	callbacks to be called.

	@param env A JNIEnv.
	*/
	void setJNIEnv(void *instance, JNIEnv *env);

	void setIOCallbacks(void *instance, jobject stdIn, jobject stdOut, jobject stdErr);
	int stdInFunction(void *callerHandle, char *buf, int len);
	int stdOutFunction(void *callerHandle, const char *str, int len);
	int stdErrFunction(void *callerHandle, const char *str, int len);

	void setPollCallback(void *instance, jobject poll);
	int pollFunction(void *instance, void *callerHandle);

	void setDisplayCallback(void *instance, jobject displayCallback);

	namespace display
	{
		int displayOpenFunction(void *instance, void *handle, void *device);
		int displayPrecloseFunction(void *instance, void *handle, void *device);
		int displayCloseFunction(void *instance, void *handle, void *device);
		int displayPresizeFunction(void *instance, void *handle, void *device, int width,
			int height, int raster, unsigned int format);
		int displaySizeFunction(void *instance, void *handle, void *device, int width,
			int height, int raster, unsigned int format,
			unsigned char *pimage);
		int displaySyncFunction(void *instance, void *handle, void *device);
		int displayPageFunction(void *instance, void *handle, void *device, int copies,
			int flush);
		int displayUpdateFunction(void *instance, void *handle, void *device, int x,
			int y, int w, int h);
		// display_memalloc omitted
		// display_memfree omitted
		int displaySeparationFunction(void *instance, void *handle, void *device,
			int component, const char *componentName, unsigned short c,
			unsigned short m, unsigned short y, unsigned short k);
		int displayAdjustBandHeightFunction(void *instance, void *handle, void *device,
			int bandHeight);
		int displayRectangleRequestFunction(void *instance, void *handle, void *device,
			void **memory, int *ox, int *oy, int *raster, int *plane_raster,
			int *x, int *y, int *w, int *h);
	}

	void setCalloutCallback(void *instance, jobject calout);
	int calloutFunction(void *instance, void *handle, const char *deviceName, int id, int size, void *data);
}