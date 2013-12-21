/* Name: hiddata.c
 * Author: Christian Starkjohann
 * Creation Date: 2008-04-11
 * Tabsize: 4
 * Copyright: (c) 2008 by OBJECTIVE DEVELOPMENT Software GmbH
 * License: GNU GPL v2 (see License.txt), GNU GPL v3 or proprietary (CommercialLicense.txt)
 */

#include <stdio.h>
#include <stdint.h>
#include <string>

#include "hiddev.h"
#include "../firmware/usbconfig.h"
#include "../firmware/prg_common.h"

#define USBOPEN_SUCCESS         0   	// no error
#define USBOPEN_ERR_ACCESS      1   	// not enough permissions to open device
#define USBOPEN_ERR_IO          2   	// I/O error
#define USBOPEN_ERR_NOTFOUND    3   	// device not found


/* ######################################################################## */
#ifdef WIN32 /* ******##################################################### */
/* ######################################################################## */

#include <windows.h>
#include <setupapi.h>

#include "hidsdi_loc.h"
#include <ddk/hidpi.h>

#ifdef DEBUG
#define DEBUG_PRINT(arg)    printf arg
#else
#define DEBUG_PRINT(arg)
#endif

/* ------------------------------------------------------------------------ */

static void convertUniToAscii(char* buffer)
{
	wchar_t* uni = (wchar_t*)buffer;
	char* ascii = buffer;

	while (*uni != 0)
	{
		*ascii++ = *uni > 0xff ? '?' : *uni;
		uni++;
	}
	
	*ascii++ = '\0';
}

std::string GetErrorString(int error)
{
	LPVOID lpMsgBuf;

	FormatMessage(	FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					NULL,
					error,
					MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
					(LPTSTR) &lpMsgBuf,
					0,
					NULL);

	std::string ret_val((char*)lpMsgBuf);

	LocalFree(lpMsgBuf);

	return ret_val;
}
		
int usbhidOpenDevice(usbDevice_t** device, int vendor, char* vendorName, int product, char* productName)
{
	GUID                                hidGuid;        	// GUID for HID driver
	HDEVINFO                            deviceInfoList;
	SP_DEVICE_INTERFACE_DATA            deviceInfo;
	SP_DEVICE_INTERFACE_DETAIL_DATA*    deviceDetails = NULL;
	DWORD                               size;
	int                                 i, openFlag = 0;	// may be FILE_FLAG_OVERLAPPED
	int                                 errorCode = USBOPEN_ERR_NOTFOUND;
	HANDLE                              handle = INVALID_HANDLE_VALUE;
	HIDD_ATTRIBUTES                     deviceAttributes;

	HidD_GetHidGuid(&hidGuid);
	deviceInfoList = SetupDiGetClassDevs(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_INTERFACEDEVICE);
	deviceInfo.cbSize = sizeof(deviceInfo);
	for (i = 0;; i++)
	{
		if (handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle);
			handle = INVALID_HANDLE_VALUE;
		}

		if (!SetupDiEnumDeviceInterfaces(deviceInfoList, 0, &hidGuid, i, &deviceInfo))
			break;  // no more entries

		// first do a dummy call just to determine the actual size required
		SetupDiGetDeviceInterfaceDetail(deviceInfoList, &deviceInfo, NULL, 0, &size, NULL);
		if (deviceDetails != NULL)
			free(deviceDetails);
		deviceDetails = (SP_DEVICE_INTERFACE_DETAIL_DATA*) malloc(size);
		deviceDetails->cbSize = sizeof(*deviceDetails);
		
		// this call is for real:
		SetupDiGetDeviceInterfaceDetail(deviceInfoList, &deviceInfo, deviceDetails, size, &size, NULL);
		DEBUG_PRINT(("checking HID path \"%s\"\n", deviceDetails->DevicePath));

		// attempt opening for R/W -- we don't care about devices which can't be accessed
		handle = CreateFile(deviceDetails->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, openFlag, NULL);
		if (handle == INVALID_HANDLE_VALUE)
		{
			DEBUG_PRINT(("opening failed: %d\n", (int)GetLastError()));
			// errorCode = USBOPEN_ERR_ACCESS; opening will always fail for mouse -- ignore
			continue;
		}
		
		deviceAttributes.Size = sizeof(deviceAttributes);
		HidD_GetAttributes(handle, &deviceAttributes);
		DEBUG_PRINT(("device attributes: vid=%d pid=%d\n", deviceAttributes.VendorID, deviceAttributes.ProductID));
		if (deviceAttributes.VendorID != vendor || deviceAttributes.ProductID != product)
			continue;   // ignore this device

		errorCode = USBOPEN_ERR_NOTFOUND;
		if (vendorName != NULL  &&  productName != NULL)
		{
			char buffer[512];
			if (!HidD_GetManufacturerString(handle, buffer, sizeof(buffer)))
			{
				DEBUG_PRINT(("error obtaining vendor name\n"));
				errorCode = USBOPEN_ERR_IO;
				continue;
			}

			convertUniToAscii(buffer);

			DEBUG_PRINT(("vendorName = \"%s\"\n", buffer));
			if (strcmp(vendorName, buffer) != 0)
				continue;

			if (!HidD_GetProductString(handle, buffer, sizeof(buffer)))
			{
				DEBUG_PRINT(("error obtaining product name\n"));
				errorCode = USBOPEN_ERR_IO;
				continue;
			}
			
			convertUniToAscii(buffer);
			
			DEBUG_PRINT(("productName = \"%s\"\n", buffer));
			if (strcmp(productName, buffer) != 0)
				continue;
		}

		break;  // we have found the device we are looking for!
	}
	
	SetupDiDestroyDeviceInfoList(deviceInfoList);
	
	if (deviceDetails != NULL)
		free(deviceDetails);
		
	if (handle != INVALID_HANDLE_VALUE)
	{
		*device = (usbDevice_t*)handle;
		errorCode = USBOPEN_SUCCESS;
	}

	return errorCode;
}

void usbhidCloseDevice(usbDevice_t* device)
{
	CloseHandle((HANDLE)device);
}

int usbhidSetReport(usbDevice_t* device, char* buffer, int len)
{
	BOOLEAN rval = HidD_SetFeature((HANDLE)device, buffer, len);
	return rval == 0 ? USBOPEN_ERR_IO : USBOPEN_SUCCESS;
}

int usbhidGetReport(usbDevice_t* device, int reportNumber, char* buffer, int* len)
{
	buffer[0] = reportNumber;
	BOOLEAN rval = HidD_GetFeature((HANDLE)device, buffer, *len);
	return rval == 0 ? USBOPEN_ERR_IO : USBOPEN_SUCCESS;
}

/* ######################################################################## */
#else /* defined WIN32 #################################################### */
/* ######################################################################## */

#include <string.h>
#include <usb.h>

#define usbDevice  usb_dev_handle		// use libusb's device structure

#define USBRQ_HID_GET_REPORT    0x01
#define USBRQ_HID_SET_REPORT    0x09

#define USB_HID_REPORT_TYPE_FEATURE 3


static int usbhidGetStringAscii(usb_dev_handle* dev, int index, char* buf, int buflen)
{
	char buffer[256];
	int rval, i;

	if ((rval = usb_get_string_simple(dev, index, buf, buflen)) >= 0)		// use libusb version if it works
		return rval;
		
	if ((rval = usb_control_msg(dev, USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) + index, 0x0409, buffer, sizeof(buffer), 5000)) < 0)
		return rval;
		
	if (buffer[1] != USB_DT_STRING)
	{
		*buf = 0;
		return 0;
	}
	
	if ((unsigned char)buffer[0] < rval)
		rval = (unsigned char) buffer[0];

	rval /= 2;
	
	// lossy conversion to ISO Latin1:
	for (i = 1; i < rval; i++)
	{
		if (i > buflen)			// destination buffer overflow
			break;
			
		buf[i - 1] = buffer[2 * i];
		
		if (buffer[2 * i + 1] != 0)		// outside of ISO Latin1 range
			buf[i - 1] = '?';
	}
	
	buf[i - 1] = 0;
	
	return i - 1;
}

int usbhidOpenDevice(usbDevice_t** device, int vendor, char* vendorName, int product, char* productName)
{
	struct usb_bus*     bus;
	struct usb_device*  dev;
	usb_dev_handle*     handle = NULL;
	int                 errorCode = USBOPEN_ERR_NOTFOUND;
	static int          didUsbInit = 0;

	if (!didUsbInit)
	{
		usb_init();
		didUsbInit = 1;
	}

	usb_find_busses();
	usb_find_devices();
	
	for (bus = usb_get_busses(); bus; bus = bus->next)
	{
		for (dev = bus->devices; dev; dev = dev->next)
		{
			if (dev->descriptor.idVendor == vendor && dev->descriptor.idProduct == product)
			{
				char    string[256];
				int     len;
				handle = usb_open(dev); // we need to open the device in order to query strings
				if (!handle)
				{
					errorCode = USBOPEN_ERR_ACCESS;
					DEBUG_PRINT(("Warning: cannot open USB device: %s\n", usb_strerror()));
					continue;
				}
				
				if (vendorName == NULL && productName == NULL)  // name does not matter
					break;

				// now check whether the names match:
				len = usbhidGetStringAscii(handle, dev->descriptor.iManufacturer, string, sizeof(string));
				if (len < 0)
				{
					errorCode = USBOPEN_ERR_IO;
					DEBUG_PRINT(("Warning: cannot query manufacturer for device: %s\n", usb_strerror()));
				} else {
					errorCode = USBOPEN_ERR_NOTFOUND;
					if (strcmp(string, vendorName) == 0)
					{
						len = usbhidGetStringAscii(handle, dev->descriptor.iProduct, string, sizeof(string));
						if (len < 0)
						{
							errorCode = USBOPEN_ERR_IO;
							DEBUG_PRINT(("Warning: cannot query product for device: %s\n", usb_strerror()));
						} else {
							errorCode = USBOPEN_ERR_NOTFOUND;
							if (strcmp(string, productName) == 0)
								break;
						}
					}
				}
				usb_close(handle);
				handle = NULL;
			}
		}
		
		if (handle)
			break;
	}
	
	if (handle != NULL)
	{
		errorCode = USBOPEN_SUCCESS;
		*device = (void*)handle;
	}
		
	return errorCode;
}

void usbhidCloseDevice(usbDevice_t* device)
{
	if (device != NULL)
		usb_close((void*)device);
}

int usbhidSetReport(usbDevice_t* device, char* buffer, int len)
{
	int bytesSent, reportId = buffer[0];

	bytesSent = usb_control_msg((void*)device, USB_TYPE_CLASS | USB_RECIP_DEVICE | USB_ENDPOINT_OUT, USBRQ_HID_SET_REPORT, USB_HID_REPORT_TYPE_FEATURE << 8 | (reportId & 0xff), 0, buffer, len, 5000);
	if (bytesSent != len)
	{
		if (bytesSent < 0)
			DEBUG_PRINT((stderr, "Error sending message: %s\n", usb_strerror()));
			
		return USBOPEN_ERR_IO;
	}
	
	return USBOPEN_SUCCESS;
}

int usbhidGetReport(usbDevice_t* device, int reportNumber, char* buffer, int* len)
{
	int bytesReceived, maxLen = *len;

	bytesReceived = usb_control_msg((void*)device, USB_TYPE_CLASS | USB_RECIP_DEVICE | USB_ENDPOINT_IN, USBRQ_HID_GET_REPORT, USB_HID_REPORT_TYPE_FEATURE << 8 | reportNumber, 0, buffer, maxLen, 5000);
	if (bytesReceived < 0)
	{
		DEBUG_PRINT(("Error sending message: %s\n", usb_strerror()));
		return USBOPEN_ERR_IO;
	}

	*len = bytesReceived;
	
	return USBOPEN_SUCCESS;
}

/* ######################################################################## */
#endif /* defined WIN32 ################################################### */
/* ######################################################################## */


HIDBurner::HIDBurner()
	: hHIDDev(NULL)
{}

HIDBurner::~HIDBurner()
{
	if (hHIDDev != NULL)
		usbhidCloseDevice(hHIDDev);
}

void HIDBurner::Open()
{
	uint8_t rawVid[2] = {USB_CFG_VENDOR_ID};
	uint8_t rawPid[2] = {USB_CFG_DEVICE_ID};
	char vendorName[] = {USB_CFG_VENDOR_NAME, 0};
	char productName[] = {USB_CFG_DEVICE_NAME, 0};
	int vid = rawVid[0] + 256 * rawVid[1];
	int pid = rawPid[0] + 256 * rawPid[1];
	
	if (usbhidOpenDevice(&hHIDDev, vid, vendorName, pid, productName) != 0)
		throw std::string("Unable to open programmer.");
}

// #define LOG_HID_TRAFFIC

void HIDBurner::ReadFirst(uint8_t* buffer)
{
	char rcvBuff[HIDREP_FIRST_BYTES + 1];
	int len = sizeof rcvBuff;
	int res = usbhidGetReport(hHIDDev, HIDREP_FIRST_ID, rcvBuff, &len);
	if (res != USBOPEN_SUCCESS)
		throw std::string("Unable to read data from programmer");

	memcpy(buffer, rcvBuff + 1, HIDREP_FIRST_BYTES);
	
#ifdef LOG_HID_TRAFFIC
	printf("-- RF ");
	for (int c = 0; c < HIDREP_FIRST_BYTES; ++c)
		printf("%02x ", buffer[c]);
	printf("\n");
#endif	
}

void HIDBurner::ReadSecond(uint8_t* buffer)
{
	char rcvBuff[HIDREP_SECOND_BYTES + 1];
	int len = sizeof rcvBuff;
	int res = usbhidGetReport(hHIDDev, HIDREP_SECOND_ID, rcvBuff, &len);
	if (res != USBOPEN_SUCCESS)
		throw std::string("Unable to read data from programmer");

	memcpy(buffer, rcvBuff + 1, HIDREP_SECOND_BYTES);

#ifdef LOG_HID_TRAFFIC
	printf("-- RS ");
	for (int c = 0; c < HIDREP_SECOND_BYTES; ++c)
		printf("%02x ", buffer[c]);
	printf("\n");
#endif	
}

void HIDBurner::WriteBytes(const uint8_t* buffer, const int bytes)
{
#ifdef LOG_HID_TRAFFIC
	printf("-- W  ");
	for (int c = 0; c < bytes; ++c)
		printf("%02x ", buffer[c]);
	printf("\n");
#endif	
	
	char sendbuff[128];
	memset(sendbuff, 0, sizeof sendbuff);
	
	// prepare the first report
	sendbuff[0] = HIDREP_FIRST_ID;
	
	if (bytes > HIDREP_FIRST_BYTES)
		memcpy(sendbuff + 1, buffer, HIDREP_FIRST_BYTES);
	else
		memcpy(sendbuff + 1, buffer, bytes);
		
	int result = usbhidSetReport(hHIDDev, sendbuff, HIDREP_FIRST_BYTES + 1);
	if (result != USBOPEN_SUCCESS)
		throw std::string("Unable to send data to programmer.");

	// if needed prepare and send the second report
	if (bytes > HIDREP_FIRST_BYTES)
	{
		memset(sendbuff, 0, sizeof sendbuff);

		sendbuff[0] = HIDREP_SECOND_ID;
		memcpy(sendbuff + 1, buffer + HIDREP_FIRST_BYTES, bytes - HIDREP_FIRST_BYTES);
		
		int result = usbhidSetReport(hHIDDev, sendbuff, HIDREP_SECOND_BYTES + 1);
		if (result != USBOPEN_SUCCESS)
			throw std::string("Unable to send data to programmer.");
	}
}