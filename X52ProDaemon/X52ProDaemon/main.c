//
//  main.c
//  X52ProDaemon
//
//  Created by James Shiell on 04/11/2014.
//  Copyright (c) 2014 Infernus. All rights reserved.
//

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <time.h>

#define APPLICATION_ID              CFSTR("org.infernus.X52ProDaemon")

#define BUFFER_SIZE                 4096

#define PROPERTY_DATE_FORMAT        CFSTR("DateFormat")
#define PROPERTY_CLOCK_TYPE         CFSTR("ClockType")
#define PROPERTY_MFD_BRIGHTNESS     CFSTR("MFDBrightness")
#define PROPERTY_LED_BRIGHTNESS     CFSTR("LEDBrightness")

#define DEFAULT_DATE_FORMAT         CFSTR("ddmmyy")
#define DEFAULT_CLOCK_TYPE          CFSTR("24")
#define DEFAULT_MFD_BRIGHTNESS      128
#define DEFAULT_LED_BRIGHTNESS      128

#define VENDOR_ID                   0x06A3
#define PRODUCT_ID                  0x0762
#define REQUEST_TYPE                0x91

#define INDEX_UPDATE_PRIMARY_CLOCK  0xc0
#define INDEX_UPDATE_DATE_DAYMONTH  0xc4
#define INDEX_UPDATE_DATE_YEAR      0xc8
#define INDEX_MFD_BRIGHTNESS        0xb1
#define INDEX_LED_BRIGHTNESS        0xb2

typedef struct DeviceData {
    io_object_t notification;
    IOUSBDeviceInterface **deviceInterface;
    dispatch_source_t dispatchSource;
    
    UInt16 lastTime;
    UInt16 lastDayMonth;
    UInt16 lastYear;
    UInt16 lastMFDBrightness;
    UInt16 lastLEDBrightness;
} DeviceData;

void SignalHandler(int sigraised);
void SendControlRequest(IOUSBDeviceInterface** usbDevice, UInt16 wValue, UInt16 wIndex);
void UpdateDate(IOUSBDeviceInterface **usbDevice, struct tm *localtimeInfo, DeviceData *deviceData);
void UpdateTime(IOUSBDeviceInterface **usbDevice, struct tm *localtimeInfo, DeviceData *deviceData);
void UpdateBrightness(IOUSBDeviceInterface **usbDevice, DeviceData *deviceData);
void TimeUpdateHandler(void* context);
void DeviceNotification(void *refCon, io_service_t service, natural_t messageType, void *messageArgument);
void DeviceAdded(void *refCon, io_iterator_t iterator);
void InitialiseDeviceData(DeviceData *deviceData);

static IONotificationPortRef gNotifyPort;
static io_iterator_t gAddedIter;
static CFRunLoopRef gRunLoop;

void SendControlRequest(IOUSBDeviceInterface** usbDevice, UInt16 wValue, UInt16 wIndex) {
    kern_return_t kr;
    
    IOUSBDevRequest request;
    request.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBVendor, kUSBDevice);
    request.bRequest = REQUEST_TYPE;
    request.wValue = wValue;
    request.wIndex = wIndex;
    request.wLength = 0;
    
    kr = (*usbDevice)->DeviceRequest(usbDevice, &request);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "DeviceRequest returned 0x%08x.\n", kr);
    }
}

void UpdateDate(IOUSBDeviceInterface **usbDevice, struct tm *localtimeInfo, DeviceData *deviceData) {
    UInt16 fields[3] = {0, 0, 0};
    UInt16 dayMonth;
    CFStringRef dateFormat;
    
    dateFormat = (CFStringRef) CFPreferencesCopyValue(PROPERTY_DATE_FORMAT, APPLICATION_ID, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    if (!dateFormat) {
        dateFormat = DEFAULT_DATE_FORMAT;
    }
    if (CFStringGetLength(dateFormat) != 6) {
        CFRelease(dateFormat);
        dateFormat = DEFAULT_DATE_FORMAT;
    }
    
    for (short i = 0; i < 3; ++i) {
        if (CFStringCompareWithOptions(dateFormat, CFSTR("dd"), CFRangeMake(i * 2, 2), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            fields[i] = localtimeInfo->tm_mday;
        } else if (CFStringCompareWithOptions(dateFormat, CFSTR("mm"), CFRangeMake(i * 2, 2), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            fields[i] = localtimeInfo->tm_mon + 1;
        } else if (CFStringCompareWithOptions(dateFormat, CFSTR("yy"), CFRangeMake(i * 2, 2), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            fields[i] = localtimeInfo->tm_year + 1900;
        }
    }
    
    dayMonth = fields[0];
    dayMonth &= ~(255 << 8);
    dayMonth |= (fields[1] << 8);
    
    if (deviceData->lastDayMonth != dayMonth) {
        SendControlRequest(usbDevice, dayMonth, INDEX_UPDATE_DATE_DAYMONTH);
        deviceData->lastDayMonth = dayMonth;
    }
    if (deviceData->lastYear != fields[2]) {
        SendControlRequest(usbDevice, fields[2], INDEX_UPDATE_DATE_YEAR);
        deviceData->lastYear = fields[2];
    }
    
    if (dateFormat != DEFAULT_DATE_FORMAT) {
        CFRelease(dateFormat);
    }
}

void UpdateTime(IOUSBDeviceInterface **usbDevice, struct tm *localtimeInfo, DeviceData *deviceData) {
    UInt16 timeValue;
    CFStringRef clockType;
    
    clockType = (CFStringRef) CFPreferencesCopyValue(PROPERTY_CLOCK_TYPE, APPLICATION_ID, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    if (clockType == NULL) {
        clockType = DEFAULT_CLOCK_TYPE;
    }
    
    timeValue = localtimeInfo->tm_min;
    
    timeValue &= ~(255 << 8);
    timeValue |= (localtimeInfo->tm_hour << 8);
    
    timeValue &= (UInt16) ~(1 << 15);
    if (CFStringCompare(clockType, CFSTR("12"), 0) != kCFCompareEqualTo) {
        timeValue |= (UInt16) (1 << 15);
    }
    
    if (deviceData->lastTime != timeValue) {
        SendControlRequest(usbDevice, timeValue, INDEX_UPDATE_PRIMARY_CLOCK);
        deviceData->lastTime = timeValue;
    }
    
    if (clockType != DEFAULT_CLOCK_TYPE) {
        CFRelease(clockType);
    }
}

void UpdateBrightness(IOUSBDeviceInterface **usbDevice, DeviceData *deviceData) {
    Boolean valueValid = 0;
    UInt16 mfdBrightness;
    UInt16 ledBrightness;
    
    mfdBrightness = CFPreferencesGetAppIntegerValue(PROPERTY_MFD_BRIGHTNESS, APPLICATION_ID, &valueValid);
    if (!valueValid || mfdBrightness < 0 || mfdBrightness > 128) {
        mfdBrightness = DEFAULT_MFD_BRIGHTNESS;
    }
    if (mfdBrightness != deviceData->lastMFDBrightness) {
        SendControlRequest(usbDevice, mfdBrightness, INDEX_MFD_BRIGHTNESS);
        deviceData->lastMFDBrightness = mfdBrightness;
    }
    
    ledBrightness = CFPreferencesGetAppIntegerValue(PROPERTY_LED_BRIGHTNESS, APPLICATION_ID, &valueValid);
    if (!valueValid || ledBrightness < 0 || ledBrightness > 128) {
        ledBrightness = DEFAULT_LED_BRIGHTNESS;
    }
    if (ledBrightness != deviceData->lastLEDBrightness) {
        SendControlRequest(usbDevice, ledBrightness, INDEX_LED_BRIGHTNESS);
        deviceData->lastLEDBrightness = ledBrightness;
    }
}

void TimeUpdateHandler(void* context) {
    DeviceData *dataRef = (DeviceData*) context;
    
    if (dataRef->deviceInterface) {
        time_t rawtime;
        struct tm *localtimeInfo;
        time(&rawtime);
        localtimeInfo = localtime(&rawtime);
        IOReturn openResult;
    
        openResult = (*dataRef->deviceInterface)->USBDeviceOpen(dataRef->deviceInterface);
        if (openResult == kIOReturnSuccess || openResult == kIOReturnExclusiveAccess) {
            UpdateTime(dataRef->deviceInterface, localtimeInfo, dataRef);
            UpdateDate(dataRef->deviceInterface, localtimeInfo, dataRef);
            UpdateBrightness(dataRef->deviceInterface, dataRef);
            
            (*dataRef->deviceInterface)->USBDeviceClose(dataRef->deviceInterface);
        }
    }
}

void InitialiseDeviceData(DeviceData *deviceData) {
    bzero(deviceData, sizeof(DeviceData));

    deviceData->lastLEDBrightness = -1;
    deviceData->lastMFDBrightness = -1;
}

void DeviceNotification(void *refCon, io_service_t service, natural_t messageType, void *messageArgument) {
    kern_return_t kr;
    DeviceData *dataRef = (DeviceData *) refCon;
    
    if (messageType == kIOMessageServiceIsTerminated) {
        if (dataRef->dispatchSource) {
            dispatch_source_cancel(dataRef->dispatchSource);
            dispatch_release(dataRef->dispatchSource);
            dataRef->dispatchSource = NULL;
        }
        
        if (dataRef->deviceInterface) {
            kr = (*dataRef->deviceInterface)->Release(dataRef->deviceInterface);
        }
        
        kr = IOObjectRelease(dataRef->notification);
        
        free(dataRef);
    }
}

void DeviceAdded(void *refCon, io_iterator_t iterator) {
    kern_return_t kr;
    io_service_t usbDevice;
    IOCFPlugInInterface **plugInInterface = NULL;
    SInt32 score;
    HRESULT res;
    
    while ((usbDevice = IOIteratorNext(iterator))) {
        DeviceData *dataRef = NULL;
        
        dataRef = malloc(sizeof(DeviceData));
        InitialiseDeviceData(dataRef);
        
        kr = IOCreatePlugInInterfaceForService(usbDevice,
                                               kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugInInterface,
                                               &score);
        
        if ((kIOReturnSuccess != kr) || !plugInInterface) {
            fprintf(stderr, "IOCreatePlugInInterfaceForService returned 0x%08x.\n", kr);
            continue;
        }
        
        res = (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                                 (LPVOID*) &dataRef->deviceInterface);
        (*plugInInterface)->Release(plugInInterface);
        
        if (res || dataRef->deviceInterface == NULL) {
            fprintf(stderr, "QueryInterface returned %d.\n", (int) res);
            continue;
        }
        
        dispatch_queue_t backgroundQueue = dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0);
        dispatch_source_t timerSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, backgroundQueue);
        dispatch_retain(timerSource);
        dispatch_source_set_timer(timerSource, dispatch_time(DISPATCH_TIME_NOW, 0), 1.0 * NSEC_PER_SEC, 1.0 * NSEC_PER_SEC);
        dispatch_source_set_event_handler_f(timerSource, TimeUpdateHandler);
        dispatch_set_context(timerSource, dataRef);
        dispatch_resume(timerSource);
        
        dataRef->dispatchSource = timerSource;
        
        kr = IOServiceAddInterestNotification(gNotifyPort,
                                              usbDevice,
                                              kIOGeneralInterest,
                                              DeviceNotification,
                                              dataRef,
                                              &(dataRef->notification));
        
        if (KERN_SUCCESS != kr) {
            fprintf(stderr, "IOServiceAddInterestNotification returned 0x%08x.\n", kr);
        }
        
        kr = IOObjectRelease(usbDevice);
    }
}

void SignalHandler(int sigraised) {
    fprintf(stderr, "Interrupted, exiting.\n");
    exit(0);
}

int main(int argc, const char *argv[]) {
    CFMutableDictionaryRef matchingDictionary = NULL;
    SInt32 idVendor = VENDOR_ID;
    SInt32 idProduct = PRODUCT_ID;
    CFRunLoopSourceRef runLoopSource;
    kern_return_t kr;
    sig_t oldHandler;
    
    oldHandler = signal(SIGINT, SignalHandler);
    if (oldHandler == SIG_ERR) {
        fprintf(stderr, "Could not establish new signal handler.");
    }
    
    matchingDictionary = IOServiceMatching(kIOUSBDeviceClassName);
    CFDictionaryAddValue(matchingDictionary,
                         CFSTR(kUSBVendorID),
                         CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &idVendor));
    CFDictionaryAddValue(matchingDictionary,
                         CFSTR(kUSBProductID),
                         CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &idProduct));

    gNotifyPort = IONotificationPortCreate(kIOMasterPortDefault);
    runLoopSource = IONotificationPortGetRunLoopSource(gNotifyPort);
    
    gRunLoop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(gRunLoop, runLoopSource, kCFRunLoopDefaultMode);
    
    kr = IOServiceAddMatchingNotification(gNotifyPort,
                                          kIOFirstMatchNotification,
                                          matchingDictionary,
                                          DeviceAdded,
                                          NULL,
                                          &gAddedIter);
    
    DeviceAdded(NULL, gAddedIter);
    
    CFRunLoopRun();
    
    fprintf(stderr, "Unexpected exit from CFRunLoopRun()\n");
    return 0;
}
