/* Kỹ thuật vào ra Completion Port:

Lựa chọn kỹ thuật nào?
• Client:
- Overlapped I/O hoặc WSAEventSelect khi cần quản lý nhiều socket
- Nếu ứng dụng có cửa sổ: WSAAsyncSelect là giải pháp tốt nhất
• Server: Overlapped I/O Completion port

--------------------------------------------------------

• Sử dụng completion port để thực hiện các thao tác vào ra overlapped
• Completion port tổ chức một hàng đợi cho các luồng và giám sát các sự kiện vào ra trên các socket
• Mỗi khi thao tác vào ra hoàn thành trên socket, completion port kích hoạt một luồng để xử lý

Hàm CreateIoCompletionPort()
• Tạo một completion port
• Nếu chưa xác định socket, truyền INVALID_HANDLE_VALUE
• Trả về: NULL nếu có lỗi, ngược lại phụ thuộc vào các giá trị truyền cho tham số
• Nếu completionPort là NULL: Completion port mới
• Nếu completionPort khác NULL: completion port với handle giống giá trị tham số
• Nếu s hợp lệ, socket được liên kết với completion port 

	HANDLE CreateIoCompletionPort (
		HANDLE s, //[IN]handle của đối tượng thực hiện thao tác vào ra
		HANDLE completionPort, //[IN] Completion port xử lý vào ra
		DWORD completionKey, //[IN] Định danh cho socket trên completion port
		DWORD numberOfThread //[IN] Số luồng sử dụng trên completion port, thường là 0
	);

* Completion Port
• B1: Tạo Completion Port với tham số thứ tư là 0(số luồng thực thi đồng thời bằng số nhân của CPU)
• B2: Xác định số nhân CPU của hệ thống
• B3: Tạo worker thread để xử lý khi thao tác vào ra hoàn thành(sử dụng số nhân CPU đã xác định ở trên)
• B4: Tạo socket và đặt vào trạng thái nghe yêu cầu
• B5: Chấp nhận yêu cầu
• B6: Tạo cấu trúc chứa dữ liệu có ý nghĩa định danh cho socket. Lưu giá trị socket ở bước 5 vào cấu trúc
• B7: Liên kết socket với completion port, truyền cấu trúc ở B6 cho tham số completionKey
• B8: Thực hiện các thao tác vào ra trên socket
• B9: Lặp các thao tác từ 5 đến 8

* Worker Thread
• Gọi hàm GetQueuedCompletionStatus() đợi thao tác vào ra hoàn thành trên completion port và 
lấy kết quả thực hiện. Trả về FALSE nếu thao tác vào ra lỗi
• Tham số lpCompletionKey và lpOverlapped chứa dữ liệu và kết quả của thao tác vào ra:
• lpCompletionKey: chứa thông tin có ý nghĩa định danh cho socket trên completion port(per-handle data)
• lpOverlapped : cấu trúc chứa đối tượng OVERLAPPED và các thông tin để worker thread xử lý dữ liệu(per-I/O data)

	BOOL GetQueuedCompletionStatus(
		HANDLE CompletionPort,
		LPDWORD lpNumberOfBytesTransferred, //[OUT] Số byte trao đổi
		PULONG_PTR lpCompletionKey, //[OUT]Định danh cho dữ liệu
		LPOVERLAPPED * lpOverlapped, //[OUT]Cấu trúc WSAOVERLAPPED
		DWORD dwMilliseconds //[IN]Thời gian đợi
	);

* Định nghĩa các cấu trúc: 

	typedef struct{
		OVERLAPPED overlapped; //Cấu trúc OVERLAPPED
		char buffer[DATA_BUFSIZE]; //Buffer chứa dữ liệu
		int bufferLen; //Kích thước buffer
		int operationType; //Loại thao tác vào ra
	} PER_IO_DATA, *LPPER_IO_DATA;

• Lưu ý: trường overlapped nên được khai báo đầu cấu trúc
• Trong trường hợp khác, sau khi gọi hàm GetQueuedCompletionStatus, 
cần sử dụng macro sau để xác định địa chỉ của vùng nhớ chứa dữ liệu cho con trỏ LPPER_IO_DATA

PCHAR CONTAINING_RECORD(PCHAR Address, TYPE Type, PCHAR Field);

* Đóng completion port:
• Mỗi completion port có thể sử dụng nhiều luồng điều khiển vào ra
• Tránh giải phóng cấu trúc OVERLAPPED trên một luồng, trong khi đang thực hiện vào ra
- Gọi hàm PostQueuedCompletionStatus() để gửi một packet có kích thước 0 tới completion port trên tất cả các luồng
- Gọi hàm CloseHandle() để đóng completion port 

BOOL PostQueuedCompletionStatus(
	HANDLE CompletionPort,
	LPDWORD lpNumberOfBytesTransferred,//[IN] Số byte trao đổi
	PULONG_PTR lpCompletionKey, //[IN]Định danh cho dữ liệu
	LPOVERLAPPED * lpOverlapped, //[IN]Cấu trúc WSAOVERLAPPED
);

 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <process.h>

#define PORT 5500
#define DATA_BUFSIZE 8192
#define SEND 0
#define RECEIVE 1

#pragma comment(lib, "Ws2_32.lib")

// Structure definition
typedef struct {
	WSAOVERLAPPED overlapped;
	WSABUF dataBuff;
	CHAR buffer[DATA_BUFSIZE];
	int bufLen;
	int recvBytes;
	int sentBytes;
	int operation;
} PER_IO_OPERATION_DATA, *LPPER_IO_OPERATION_DATA;

typedef struct {
	SOCKET socket;
} PER_HANDLE_DATA, *LPPER_HANDLE_DATA;

unsigned __stdcall serverWorkerThread(LPVOID CompletionPortID);

int main()
{
	SOCKADDR_IN internetAddr;
	SOCKET listenSock, acceptSock;
	HANDLE completionPort;
	SYSTEM_INFO systemInfo;
	LPPER_HANDLE_DATA perHandleData;
	LPPER_IO_OPERATION_DATA perIoData;
	DWORD transferredBytes;
	DWORD flags;
	WSADATA wsaData;


	if (WSAStartup((2, 2), &wsaData) != 0) {
		printf("WSAStartup() failed with error %d\n", GetLastError());
		return 1;
	}

	// Setup an I/O completion port
	if ((completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)) == NULL) {
		printf("CreateIoCompletionPort() failed with error %d\n", GetLastError());
		return 1;
	}

	// Determine how many processors are on the system
	GetSystemInfo(&systemInfo);

	// Create worker threads based on the number of processors available on the
	// system. Create two worker threads for each processor	
	for (int i = 0; i < (int)systemInfo.dwNumberOfProcessors * 2; i++) {
		// Create a server worker thread and pass the completion port to the thread
		if (_beginthreadex(0, 0, serverWorkerThread, (void*)completionPort, 0, 0) == 0) {
			printf("Create thread failed with error %d\n", GetLastError());
			return 1;
		}
	}
	// Create a listening socket
	if ((listenSock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED)) == INVALID_SOCKET) {
		printf("WSASocket() failed with error %d\n", WSAGetLastError());
		return 1;
	}
	internetAddr.sin_family = AF_INET;
	internetAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	internetAddr.sin_port = htons(PORT);
	if (bind(listenSock, (PSOCKADDR)&internetAddr, sizeof(internetAddr)) == SOCKET_ERROR) {
		printf("bind() failed with error %d\n", WSAGetLastError());
		return 1;
	}

	// Prepare socket for listening
	if (listen(listenSock, 5) == SOCKET_ERROR) {
		printf("listen() failed with error %d\n", WSAGetLastError());
		return 1;
	}

	// Accept connections and assign to the completion port
	while (TRUE) {
		if ((acceptSock = WSAAccept(listenSock, NULL, NULL, NULL, 0)) == SOCKET_ERROR) {
			printf("WSAAccept() failed with error %d\n", WSAGetLastError());
			return 1;
		}
		// Create a socket information structure to associate with the socket
		if ((perHandleData = (LPPER_HANDLE_DATA)GlobalAlloc(GPTR, sizeof(PER_HANDLE_DATA))) == NULL) {
			printf("GlobalAlloc() failed with error %d\n", GetLastError());
			return 1;
		}

		// Associate the accepted socket with the original completion port
		printf("Socket number %d got connected...\n", acceptSock);
		perHandleData->socket = acceptSock;
		if (CreateIoCompletionPort((HANDLE)acceptSock, completionPort, (DWORD)perHandleData, 0) == NULL) {
			printf("CreateIoCompletionPort() failed with error %d\n", GetLastError());
			return 1;
		}
		// Create per I/O socket information structure to associate with the WSARecv call below
		if ((perIoData = (LPPER_IO_OPERATION_DATA)GlobalAlloc(GPTR, sizeof(PER_IO_OPERATION_DATA))) == NULL) {
			printf("GlobalAlloc() failed with error %d\n", GetLastError());
			return 1;
		}
		ZeroMemory(&(perIoData->overlapped), sizeof(OVERLAPPED));
		perIoData->sentBytes = 0;
		perIoData->recvBytes = 0;
		perIoData->dataBuff.len = DATA_BUFSIZE;
		perIoData->dataBuff.buf = perIoData->buffer;
		perIoData->operation = RECEIVE;
		flags = 0;

		if (WSARecv(acceptSock, &(perIoData->dataBuff), 1, &transferredBytes, &flags, &(perIoData->overlapped), NULL) == SOCKET_ERROR) {
			if (WSAGetLastError() != ERROR_IO_PENDING) {
				printf("WSARecv() failed with error %d\n", WSAGetLastError());
				return 1;
			}
		}
	}
	return 0;
}

unsigned __stdcall serverWorkerThread(LPVOID completionPortID)
{
	HANDLE completionPort = (HANDLE)completionPortID;
	DWORD transferredBytes;
	LPPER_HANDLE_DATA perHandleData;
	LPPER_IO_OPERATION_DATA perIoData;
	DWORD flags;
	while (TRUE) {
		if (GetQueuedCompletionStatus(completionPort,
			&transferredBytes,
			(LPDWORD)&perHandleData,
			(LPOVERLAPPED *)&perIoData,
			INFINITE) == 0) {
			printf("GetQueuedCompletionStatus() failed with error %d\n", GetLastError());
			return 0;
		}
		// Check to see if an error has occurred on the socket and if so
		// then close the socket and cleanup the SOCKET_INFORMATION structure
		// associated with the socket
		if (transferredBytes == 0 && (perIoData->operation == SEND || perIoData->operation == RECEIVE)) {
			printf("Closing socket %d\n", perHandleData->socket);
			if (closesocket(perHandleData->socket) == SOCKET_ERROR) {
				printf("closesocket() failed with error %d\n", WSAGetLastError());
				return 0;
			}
			GlobalFree(perHandleData);
			GlobalFree(perIoData);
			continue;
		}
		// Check to see if the operation field equals RECEIVE. If this is so, then
		// this means a WSARecv call just completed so update the recvBytes field
		// with the transferredBytes value from the completed WSARecv() call
		if (perIoData->operation == RECEIVE) {
			perIoData->recvBytes = transferredBytes;
			perIoData->sentBytes = 0;
			perIoData->operation = SEND;
		}
		else if (perIoData->operation == SEND) {
			perIoData->sentBytes += transferredBytes;
		}

		if (perIoData->recvBytes > perIoData->sentBytes) {
			// Post another WSASend() request.
			// Since WSASend() is not guaranteed to send all of the bytes requested,
			// continue posting WSASend() calls until all received bytes are sent.
			ZeroMemory(&(perIoData->overlapped), sizeof(OVERLAPPED));
			perIoData->dataBuff.buf = perIoData->buffer + perIoData->sentBytes;
			perIoData->dataBuff.len = perIoData->recvBytes - perIoData->sentBytes;
			perIoData->operation = SEND;

			if (WSASend(perHandleData->socket,
				&(perIoData->dataBuff),
				1,
				&transferredBytes,
				0,
				&(perIoData->overlapped),
				NULL) == SOCKET_ERROR) {
				if (WSAGetLastError() != ERROR_IO_PENDING) {
					printf("WSASend() failed with error %d\n", WSAGetLastError());
					return 0;
				}
			}
		}
		else {
			// No more bytes to send post another WSARecv() request
			perIoData->recvBytes = 0;
			perIoData->operation = RECEIVE;
			flags = 0;
			ZeroMemory(&(perIoData->overlapped), sizeof(OVERLAPPED));
			perIoData->dataBuff.len = DATA_BUFSIZE;
			perIoData->dataBuff.buf = perIoData->buffer;
			if (WSARecv(perHandleData->socket,
				&(perIoData->dataBuff),
				1,
				&transferredBytes,
				&flags,
				&(perIoData->overlapped), NULL) == SOCKET_ERROR) {
				if (WSAGetLastError() != ERROR_IO_PENDING) {
					printf("WSARecv() failed with error %d\n", WSAGetLastError());
					return 0;
				}
			}
		}
	}
}