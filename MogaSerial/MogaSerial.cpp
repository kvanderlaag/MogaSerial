/*
Moga serial to vJoy interface

Copyright (c) Jake Montgomery.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:
    MogaSerial.cpp
    
Abstract:
	Monolithic code file for the Moga serial to vJoy interface program.

Environment:
    kernel mode and User mode

Notes:
	Things aren't very OO friendly yet.  Could matter if/when threaded and GUI-enabled.

Revision History:
	x.9.6 - Testing the SCP driver as a replacement for vJoy.
	0.9.5 - Boosted thread priority to try and resolve latency issues
	0.9.4 - Fixed controller state not being properly zero'd on init and disconnect.
	0.9.3 - vJoy device id selection routine - added by badfontkeming@gmail.com
	0.9.2 - Switched to passive listening mode for controller updates.
	        Reduced active polling to once every two seconds to check for disconnects.
	        This should reduce bluetooth network traffic and prevent any chance of missed inputs.
	0.9.0 - First semi-public release.
	0.x.x - Test builds and device probing.
*/


// Link to ws2_32.lib
#include	"MogaSerial.h"


//Basic method to prompt the user for which vJoy ID to use
//Separating this as a method to keep things clean-ish

//my c++ class always taught me to pipe things straight from cin
//but I think that's awful so I'll be copying your code :)
int promptVJoyID(void)
{
	printf("Please enter the desired vJoy controller slot: [1-16] (def. 1)\n");
	char buf[5000];
	int device = -1;
	int retVal;
	//no input validation loop; we'll be defaulting to 1.
	printf("> ");
	fgets(buf, sizeof(buf), stdin);
	retVal = sscanf_s(buf, "%d", &device);

	if (device < 1 || device > 16 || retVal != 1)
	{
		device = 1;
		printf("Defaulting to 1.\n");
	}
	return device;
}


BOOL FindMogaAddress(MOGA_DATA *pMogaData)
{
	union {
		CHAR buf[5000];
		SOCKADDR_BTH _Unused_;  // properly align buffer to BT_ADDR requirements
	};
	HANDLE hLookup;
	LPWSAQUERYSET pwsaQuery;
	DWORD dwSize, BTSA_size;
	BT_ADDR btAddrList[26];
	char BTStrAddr[40];
	int i = 0, j = -1, retVal;
           
	// configure the queryset structure
	pwsaQuery = (LPWSAQUERYSET) buf;
	dwSize  = sizeof(buf);
	ZeroMemory(pwsaQuery, dwSize);
	pwsaQuery->dwSize = dwSize;
	pwsaQuery->dwNameSpace = NS_BTH;  // namespace MUST be NS_BTH for bluetooth queries
 
	// initialize searching procedure
	if (WSALookupServiceBegin(pwsaQuery, LUP_CONTAINERS, &hLookup) == SOCKET_ERROR)
	{
		printf("WSALookupServiceBegin() failed %d\r\n", WSAGetLastError());
		return -6;
	}

	printf("Found the following bluetooth devices.\n");
	printf("Select the one that matches your Moga in MODE A.\n\n");
	// iterate through all found devices, returning name and address.
	// if they have over 25 paired devices on a gaming machine, they're crazy.
	while (WSALookupServiceNext(hLookup, LUP_RETURN_NAME | LUP_RETURN_ADDR, &dwSize, pwsaQuery) == 0 && i < 25)
	{
		BTSA_size = sizeof(BTStrAddr);
		ZeroMemory(BTStrAddr, BTSA_size);
		WSAAddressToString(pwsaQuery->lpcsaBuffer->RemoteAddr.lpSockaddr, sizeof(SOCKADDR_BTH), NULL, (LPWSTR)BTStrAddr, &BTSA_size);
		printf("%2d - %ls  %ls\n", ++i, pwsaQuery->lpszServiceInstanceName, BTStrAddr);
		btAddrList[i] = ((SOCKADDR_BTH *)pwsaQuery->lpcsaBuffer->RemoteAddr.lpSockaddr)->btAddr;
	}

	WSALookupServiceEnd(hLookup);

	// I would prefer to parse the bluetooth names and auto-select, but...
	// I don't know how anything but the Power Pro identify themselves.
	do	{
		printf("> ");
		fgets(buf, sizeof(buf), stdin);
		retVal = sscanf_s(buf, "%d", &j);
	} while (j < 1 || j > i || retVal!=1);
	pMogaData->Addr = btAddrList[j];

	return 1;
}
 

// Moga MODE A command codes discovered so far:
// -Sent TO Moga controller
//    65 - poll contoller state, digital triggers  (12b response)
//    67 - change controller id
//    68 - listen mode, digital triggers  (12b response)
//    69 - poll controller state, analog triggers  (14b response)
//    70 - listen mode, analog triggers  (14b response)
// -Recv FROM Moga controller
//    97 - poll command response, digital triggers  (12b response)
//   100 - listen mode status update, digital triggers  (12b response)
//   101 - poll command response, analog triggers  (14b response)
//   102 - listen mode status update, analog triggers  (14b response)
// Oddly, there seems to be no way to obtain battery status.  It's reported in HID Mode B, but not here.

// TODO: Find out if the official Moga app is aware of the battery status of the controller.
// TODO: Change the controller's lights to reflect the vJoy ID selected.
int MogaSendMsg(MOGA_DATA *pMogaData, unsigned char code)
{
	uint8_t i, chksum = 0;
	char msg[SENDMSG_LEN];
	msg[0] = 0x5a;            // identifier
	msg[1] = SENDMSG_LEN;     // message length - always 5
	msg[2] = code;            // command to send
	msg[3] = pMogaData->CID;  // controller id
	for (i = 0; i < SENDMSG_LEN-1; i++)
		chksum = msg[i] ^ chksum;
	msg[4] = chksum;
	send(pMogaData->Socket, msg, SENDMSG_LEN, 0);
	return 1;
}


// Received messages are a similar format to the sent messages:
//   byte 0 - 0x71 identifier 
//   byte 1 - length, 12 or 14
//   byte 2 - message code
//   byte 3 - controller id
//   4 - 9 or 11 - data bytes
//   10 or 12    - 0x10 ..not sure what this means.  Could be identifying the kind of Moga.
//   11 or 13    - checksum
// If the message doesn't validate, something is messed up.  Just reset the connection.
int MogaGetMsg(MOGA_DATA *pMogaData)
{
	int retVal;
	uint8_t i, chksum = 0, recvmsg_len = 14;
	unsigned char recvbuf[RECVBUF_LEN];

	// Returned data can be 12 or 14 bytes long, so the message length should be checked before a full read.
	// I dislike making assumptions on socket reads, but in the interests of streamlining things as much as possible
	// to maybe cut down on lag, and since we do know what the length will be, I'll hardcode the recv message length.
	retVal = recv(pMogaData->Socket, (char *)recvbuf, recvmsg_len, 0);
//	if (retVal == 2)
//	{
//		recvmsg_len = recvbuf[1];
//		retVal = recv(pMogaData->Socket, (char *)recvbuf+2, recvmsg_len-2, 0);
//	}
//	else   
	if (retVal != recvmsg_len)
		return -1;    // Recv socket error or timeout
	for (i = 0; i < recvmsg_len-1; i++)
		chksum = recvbuf[i] ^ chksum;
	if (recvbuf[0] != 0x7a || recvbuf[recvmsg_len-1] != chksum)
		return -2;    // Received bad data
	memcpy(pMogaData->State, recvbuf+4, MOGABUF_LEN);
	//PrintBuf(recvbuf);
	return 1;
}


// Testing// Testing// Testing// Testing// Testing// Testing
void PrintBuf(unsigned char *buf)
{
	int i;
	SYSTEMTIME t;

	GetSystemTime(&t);
	printf ("[.%03d] ", t.wMilliseconds);
	for (i=0; i<buf[1]; i++)
		printf("%02x ", buf[i]);
	printf("\n");
}


int MogaConnect(MOGA_DATA *pMogaData)
{
	SOCKADDR_BTH sockAddr;
	DWORD timeout = 2000;
	int retVal;

	pMogaData->Socket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
	setsockopt(pMogaData->Socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(DWORD));
	memset (&sockAddr, 0, sizeof(sockAddr));
	sockAddr.addressFamily = AF_BTH;
	sockAddr.serviceClassId = SerialPortServiceClass_UUID;
	sockAddr.port = 0;  
	sockAddr.btAddr = pMogaData->Addr;

	printf("\nAttempting connection ... ");
	retVal = connect(pMogaData->Socket, (SOCKADDR*)&sockAddr, sizeof(sockAddr));
	if (retVal)
	{
		printf("Failed.  (%d)\n", GetLastError());
		return 0;
	}
	printf("Success.  Press <Ctrl-C> to quit.\n");
	MogaSendMsg(pMogaData, 67);
	return 1;
}


void MogaReset(MOGA_DATA *pMogaData)
{
	closesocket(pMogaData->Socket);
	pMogaData->Socket = INVALID_SOCKET;
	memset(pMogaData->State, 0, MOGABUF_LEN);
	//vJoyUpdate(pMogaData);
	SCP_Update(pMogaData);
	if (KeepGoing)
	{
		printf("Reconnecting in 3 seconds.\n");
		Sleep(3000);
	}
}


// The Moga Power Pro response speed usually seems to be 100 times a second, same as its message rate in HID mode.
// Occasionally the timestamps appear faster.  Not sure if this is due to my bluetooth adapter or something else.
// The Moga seems to sometimes ignore command message 70 after a connection.
// Not sure why, but sending it again a second later seems to work.
void MogaListener(MOGA_DATA *pMogaData)
{
	int retVal=1;

	MogaSendMsg(pMogaData, 69);  // Get initial controller state
	MogaSendMsg(pMogaData, 70);  // Enable listen mode
	while (retVal == 1 && KeepGoing)
	{
		retVal = MogaGetMsg(pMogaData);
		if (retVal < 1)
		{
			// Listen timeout.  Poll for status and to check connection state.
			MogaSendMsg(pMogaData, 69);
			retVal = MogaGetMsg(pMogaData);
			if (retVal < 1)
				printf("Moga disconnected.  (%d)\n", GetLastError());
			else
				MogaSendMsg(pMogaData, 70);  // Re-enable listen mode if the first try failed.
		}
		//vJoyUpdate(pMogaData);
		SCP_Update(pMogaData);
	}
}


// The new 2.1.6 vJoy dll won't work with older versions of vJoy drivers.
// Fortunately the old 2.0.5 vJoy dll works with new versions, despite giving an error message.
/*int vJoySetup(MOGA_DATA *pMogaData)
{
	WORD VerDll, VerDrv;
	if (!vJoyEnabled())
	{
		printf("vJoy driver not enabled: Failed Getting vJoy attributes.\n");
		return -2;
	}
	if (!DriverMatch(&VerDll, &VerDrv))
		printf("vJoy Driver (version %04x) does not match vJoyInterface DLL (version %04x)\n", VerDrv ,VerDll);

	// Get the state of the requested device
	VjdStat status = GetVJDStatus(pMogaData->vJoyInt);
	switch (status)
	{
	case VJD_STAT_OWN:
		break;
	case VJD_STAT_FREE:
		break;
	case VJD_STAT_BUSY:
		printf("vJoy Device %d is already owned by another feeder\nCannot continue\n", pMogaData->vJoyInt);
		return -3;
	case VJD_STAT_MISS:
		printf("vJoy Device %d is not installed or disabled\nCannot continue\n", pMogaData->vJoyInt);
		return -4;
	default:
		printf("vJoy Device %d general error\nCannot continue\n", pMogaData->vJoyInt);
		return -1;
	};

	// Acquire the target
	if ((status == VJD_STAT_OWN) || ((status == VJD_STAT_FREE) && (!AcquireVJD(pMogaData->vJoyInt))))
	{
		printf("Failed to acquire vJoy device number %d.\n", pMogaData->vJoyInt);
		return -1;
	}
	printf("vJoy %S enabled, attached to device %d.\n\n", (wchar_t *)GetvJoySerialNumberString(), pMogaData->vJoyInt);
	memset(pMogaData->State, 0, MOGABUF_LEN);
	vJoyUpdate(pMogaData);

	return 1;
}*/


// hid default - A=1 B=2 X=3 Y=4 L1=5 R1=6 SEL=7 START=8 L3=9 R3=10
// raw - 0000 0000  0000 0000  0000 0000  0000 0000  0000 0000  0000 0000  0000 0000  0000 0000
//       RLSS XABY  RLRL HHHH  left stk   left stk   right stk  right stk    left       right
//       11et       3322 EWSN   x axis     y axis     x axis     y axis     trigger    trigger
// Triggers are reported as both buttons and axis.
// Thumbsticks report 00 at neutral, need to be corrected.
// Buttons are so, so scrambled from what's "expected".
/*void vJoyUpdate(MOGA_DATA *pMogaData)
{
	JOYSTICK_POSITION vJoyData;

	vJoyData.bDevice = (BYTE)pMogaData->vJoyInt;

	vJoyData.lButtons = 0;
	vJoyData.lButtons |= (((pMogaData->State[0] >> 2) & 1) << 0);  // A
	vJoyData.lButtons |= (((pMogaData->State[0] >> 1) & 1) << 1);  // B
	vJoyData.lButtons |= (((pMogaData->State[0] >> 3) & 1) << 2);  // X
	vJoyData.lButtons |= (((pMogaData->State[0] >> 0) & 1) << 3);  // Y
	vJoyData.lButtons |= (((pMogaData->State[0] >> 6) & 1) << 4);  // L1
	vJoyData.lButtons |= (((pMogaData->State[0] >> 7) & 1) << 5);  // R1
	vJoyData.lButtons |= (((pMogaData->State[0] >> 5) & 1) << 6);  // Select
	vJoyData.lButtons |= (((pMogaData->State[0] >> 4) & 1) << 7);  // Start
	vJoyData.lButtons |= (((pMogaData->State[1] >> 6) & 1) << 8);  // L3
	vJoyData.lButtons |= (((pMogaData->State[1] >> 7) & 1) << 9);  // R3
	
	switch((pMogaData->State[1] & 0x0F))
	{
	case 0x01:  vJoyData.bHats = 0;     break;  // Hat N
	case 0x09:  vJoyData.bHats = 4500;  break;  // Hat NE
	case 0x08:  vJoyData.bHats = 9000;  break;  // Hat E
	case 0x0A:  vJoyData.bHats = 13500; break;  // Hat SE
	case 0x02:  vJoyData.bHats = 18000; break;  // Hat S
	case 0x06:  vJoyData.bHats = 22500; break;  // Hat SW
	case 0x04:  vJoyData.bHats = 27000; break;  // Hat W
	case 0x05:  vJoyData.bHats = 31500; break;  // Hat NW
	default:    vJoyData.bHats = -1;
	}

	vJoyData.wAxisX = FixAxis(pMogaData->State[2]) * 128;
	vJoyData.wAxisY = (0xff - FixAxis(pMogaData->State[3])) * 128;  //invert
	vJoyData.wAxisXRot = FixAxis(pMogaData->State[4]) * 128;
	vJoyData.wAxisYRot = (0xff - FixAxis(pMogaData->State[5])) * 128;  //invert
	vJoyData.wAxisZ = pMogaData->State[6] * 128;
	vJoyData.wAxisZRot = pMogaData->State[7] * 128;
		
	UpdateVJD(pMogaData->vJoyInt, (PVOID)&vJoyData);
}*/


// Get handle to SCP virtual device driver
// Most of this is extracted from working source code in other projects.  Unfortunately with no
// documentation for SCP, I'm not sure if there's any better way of doing this.
int SCP_Setup(MOGA_DATA *pMogaData)
{
	// {F679F562-3164-42CE-A4DB-E7DDBE723909} - GUID for main SCP device interface
	GUID Target = { 0xF679F562, 0x3164, 0x42CE, {0xA4, 0xDB, 0xE7, 0xDD, 0xBE, 0x72, 0x39, 0x09}};
	wchar_t Path[256];
	char *buf;
	DWORD bufferSize;
	HDEVINFO deviceInfoSet;
	SP_DEVICE_INTERFACE_DATA DeviceInterfaceData;
	PSP_DEVICE_INTERFACE_DETAIL_DATA pDeviceDetailData;
	SP_DEVINFO_DATA DevInfoData;
	int retVal = 1;

	DeviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
	DevInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

	deviceInfoSet = SetupDiGetClassDevs(&Target, 0, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (SetupDiEnumDeviceInterfaces(deviceInfoSet, 0, &Target, 0, &DeviceInterfaceData))
	{
		SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &DeviceInterfaceData, 0, 0, &bufferSize, &DevInfoData);

		buf = (char *)malloc(sizeof(char) * bufferSize);
		pDeviceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)buf;
		pDeviceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		if (SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &DeviceInterfaceData, pDeviceDetailData, bufferSize, &bufferSize, &DevInfoData))
			wcscpy_s(Path, pDeviceDetailData->DevicePath);
		else
		{
			printf("Error: %d\n SCP Driver not active?\n", GetLastError());
			retVal = -2;
		}
		free(buf);
	}
	else
	{
		printf("Error: %d\n SCP Driver not installed?\n", GetLastError());
		retVal = -4;
	}
	
	if (retVal == 1)
	{
		pMogaData->SCP_Handle = CreateFile(Path, (GENERIC_WRITE | GENERIC_READ), FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, 0);
		if (pMogaData->SCP_Handle == INVALID_HANDLE_VALUE)
		{
			printf("Couldn't attach to SCP driver.\n");
			retVal = -1;
		}
	}
	
	if (SCP_OnOff(pMogaData, true) == 0)
		printf("Warning - Controller ID %08X already attached.\n", pMogaData->Addr);
	return retVal;
}


// Trigger and axis data from the Moga is apparently exactly what an Xbox360 controller expects.
// Pressing start + select together seems like a good way to emulate the Xbox guide button.
// Again, the data structure and update command are pulled from working source code.
// I've no idea what the control codes or unused bytes are doing, or could do.
void SCP_Update(MOGA_DATA *pMogaData)
{
	unsigned char Data[28];
	DWORD Transfered;
	memset(Data, 0, sizeof(Data));
	Data[0] = 0x1C;
	Data[4] = ((pMogaData->Addr >> 0) & 0xFF);
	Data[5] = ((pMogaData->Addr >> 8) & 0xFF);
	Data[6] = ((pMogaData->Addr >> 16) & 0xFF);
	Data[7] = ((pMogaData->Addr >> 24) & 0xFF);
	Data[9] = 0x14;

	Data[11] |= (((pMogaData->State[0] >> 5) & 
		          (pMogaData->State[0] >> 4) & 1) << 2);  // Guide
	Data[11] |= (((pMogaData->State[0] >> 6) & 1) << 0);  // L1
	Data[11] |= (((pMogaData->State[0] >> 7) & 1) << 1);  // R1
	Data[11] |= (((pMogaData->State[0] >> 2) & 1) << 4);  // A
	Data[11] |= (((pMogaData->State[0] >> 1) & 1) << 5);  // B
	Data[11] |= (((pMogaData->State[0] >> 3) & 1) << 6);  // X
	Data[11] |= (((pMogaData->State[0] >> 0) & 1) << 7);  // Y
	Data[10] |= (((pMogaData->State[0] >> 4) & 1) << 4);  // Start
	Data[10] |= (((pMogaData->State[0] >> 5) & 1) << 5);  // Select
	Data[10] |= (((pMogaData->State[1] >> 6) & 1) << 6);  // L3
	Data[10] |= (((pMogaData->State[1] >> 7) & 1) << 7);  // R3

	Data[10] |= pMogaData->State[1] & 0x0f;  // Dpad
	
	Data[15] = pMogaData->State[2];  // Left X-axis
	Data[17] = pMogaData->State[3];  // Left Y-axis
	Data[19] = pMogaData->State[4];  // Right X-axis
	Data[21] = pMogaData->State[5];  // Right Y-axis
	Data[12] = pMogaData->State[6];  // Left Trigger
	Data[13] = pMogaData->State[7];  // Right Trigger

	DeviceIoControl(pMogaData->SCP_Handle, 0x2A400C, Data, sizeof(Data), 0, 0, &Transfered, 0);
}


// Make the system think an xbox360 contoller has been plugged in or removed.  Unfortunately,
// there doesn't seem to be a way of ensuring nobody else is using that controller when unplugging it.
// SCP doesn't report the xinput device number, so we have to compare the xinput state before and after
// attaching the virtual pad to see which number we've been assigned.
// The Moga won't respond with a CID of 0.  5 turns off the blue lights but still functions, works as a good error mode.
int SCP_OnOff(MOGA_DATA *pMogaData, bool connect)
{
	unsigned char Data[16];
	int i, retVal;
	XINPUT_STATE xState;
	DWORD Transfered, xConnected[4];
	memset(Data, 0, sizeof(Data));
	Data[0] = 0x10;
	Data[4] = ((pMogaData->Addr >> 0) & 0xFF);
	Data[5] = ((pMogaData->Addr >> 8) & 0xFF);
	Data[6] = ((pMogaData->Addr >> 16) & 0xFF);
	Data[7] = ((pMogaData->Addr >> 24) & 0xFF);

	memset(pMogaData->State, 0, MOGABUF_LEN);  // zero the controller on connect/disconnect
	if (connect)
	{
		for (i = 0; i < 4; i++)  // xinput connection status pre-attach
			xConnected[i] = XInputGetState(i, &xState);
		retVal = DeviceIoControl(pMogaData->SCP_Handle, 0x2A4000, Data, sizeof(Data), 0, 0, &Transfered, 0); // plugin
		SCP_Update(pMogaData);  // update to zero
		Sleep(100);
		for (i = 0; i < 4; i++)  // xinput connection status post-attach
			if (xConnected[i] != XInputGetState(i, &xState))
				break;
		pMogaData->CID = i+1;  // The blue Moga lights now mean something!
	}
	else
	{
		SCP_Update(pMogaData);  // update to zero
		Sleep(100);
		retVal = DeviceIoControl(pMogaData->SCP_Handle, 0x2A4004, Data, sizeof(Data), 0, 0, &Transfered, 0); // unplug
	}
	return retVal;
}


BOOL intHandler(int sig)
{
	KeepGoing = false;
	printf("\nExiting...\n");
	SCP_OnOff(&MogaData, false);
	return true;
}


int main(int argc, char **argv)
{
	WSADATA wsd;
	int retVal;

	// Boosting thread priority by 2 to combat input lag on some systems.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	SetConsoleCtrlHandler((PHANDLER_ROUTINE)intHandler, TRUE);
	SetConsoleTitle(_T("MogaSerial"));

	printf("-------------------------------------------\n");
	printf("  Moga serial to SCP interface    v x.9.6  \n");
	printf("-------------------------------------------\n");

	if(WSAStartup(MAKEWORD(2,2), &wsd) != 0)
	{
		printf("WSAStartup() failed with error code %ld\n", WSAGetLastError());
		Sleep(5000);
		exit(-5);
	}

	retVal = FindMogaAddress(&MogaData);
	if (retVal < 1 || !KeepGoing)
	{
		WSACleanup();
		exit(retVal);
	}

	//MogaData.vJoyInt = promptVJoyID();
	//retVal = vJoySetup(&MogaData);
	retVal = SCP_Setup(&MogaData);
	if (retVal < 1)
	{
		Sleep(5000);
		exit(retVal);
	}
	if (MogaData.CID < 5)
		printf("\nAttached as XInput controller %d\n", MogaData.CID);
	else
		printf("\nCouldn't determine XInput controller number.\n", MogaData.CID);

	while(KeepGoing)
	{
		retVal = MogaConnect(&MogaData);
		if (retVal)
			MogaListener(&MogaData);
		// MogaListener only returns on an error or ctrl-c.
		MogaReset(&MogaData);
	}
 
	WSACleanup();
	//RelinquishVJD(MogaData.vJoyInt);
	SCP_OnOff(&MogaData, false);
	Sleep(500);

	return retVal;
}