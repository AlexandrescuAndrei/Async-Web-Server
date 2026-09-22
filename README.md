# Asynchronous HTTP Server in C

An asynchronous HTTP server implemented in **C** using low-level Linux networking and I/O mechanisms.

The project focuses on the internal architecture of an event-driven web server and on how multiple types of I/O can be coordinated without relying on a high-level networking framework. It uses TCP sockets for communication, non-blocking client connections, `epoll` for event monitoring, `sendfile` for efficient static file transfers, and Linux asynchronous I/O together with `eventfd` for dynamic resources.

Instead of hiding the networking details behind an existing server library, the implementation works directly with sockets, file descriptors, HTTP parsing, connection states, file operations, and kernel interfaces. This makes the project mainly an exercise in systems programming and in understanding how asynchronous servers are built at a lower level.

## Server Architecture

The application starts by creating a TCP socket, enabling address reuse, binding it to port `8888`, and placing it in listening mode.

The listening socket is registered with an `epoll` instance, which becomes the central mechanism used by the server to wait for activity. When a new client connects, the connection is accepted and its socket is configured as non-blocking using `fcntl` and the `O_NONBLOCK` flag.

Each client is represented by a dedicated connection structure containing the socket descriptor, requested file information, HTTP parser state, receive and send buffers, file position, asynchronous I/O structures, an `eventfd`, and the current state of the connection.

This allows all information related to a request to remain associated with its connection while the server moves between different stages of processing.

The main server loop waits for events using `epoll_wait()` and reacts when either a new connection arrives or an existing connection has work that can be processed.

## HTTP Request Processing

Data received from a client is stored in a connection-specific receive buffer and passed to an HTTP parser.

The project uses an HTTP parser callback to extract the requested path from the incoming request. Once a valid path has been found, the request can move to the next stage and the server attempts to locate the corresponding file.

Requested resources are divided into two categories based on their path: `/static/` and `/dynamic/`.

The server constructs the corresponding file name and attempts to open the requested resource. The implementation serves `.dat` files, converting the requested file extension to `.dat` when necessary.

If the file is found, its size is obtained using `fstat()` and the server prepares an HTTP `200 OK` response containing the appropriate `Content-Length`.

If the requested file cannot be opened or the resource is invalid, the connection is prepared for an HTTP `404 Not Found` response instead.

The connection is closed after the response has been completed.

## Event-Driven Connection Management

A central part of the project is the use of an explicit connection state machine.

Rather than processing an entire request through one long blocking sequence, every connection stores its current state and progresses through the server depending on what operation has completed.

The available states represent stages such as receiving request data, processing a received request, sending the HTTP header, performing an asynchronous operation, sending file data, handling a `404` response, and finally closing the connection.

Examples include `STATE_INITIAL`, `STATE_REQUEST_RECEIVED`, `STATE_SENDING_HEADER`, `STATE_ASYNC_ONGOING`, `STATE_SENDING_DATA`, and `STATE_CONNECTION_CLOSED`.

Using explicit states makes it easier to separate the different parts of the request lifecycle and coordinate socket communication with file I/O.

This approach is particularly useful in an event-driven server because an operation may not be completed immediately. The server has to remember where a connection stopped and continue processing it when the corresponding event becomes available.

## Static and Dynamic File Transfers

The server uses different strategies depending on the type of resource requested by the client.

Static resources are transferred using the Linux `sendfile()` system call.

Instead of repeatedly reading data from a file into a user-space buffer and then sending that buffer through the socket, `sendfile()` allows file contents to be transferred more directly between file descriptors.

This provides a simple and efficient path for resources requested from the `/static/` directory.

Dynamic resources use a different mechanism based on Linux asynchronous I/O.

For these requests, the server prepares an asynchronous file read and submits it using `io_submit()`. The operation reads file data into the connection's send buffer without following the same blocking file-read approach that would normally be used with `read()`.

Once a portion of the file has been loaded, the contents of the buffer are sent through the client socket. If additional data remains, another asynchronous read can be started until the entire file has been processed.

Having separate static and dynamic paths makes the project a useful comparison between `sendfile()` and explicit asynchronous file I/O.

## Linux AIO and eventfd

Dynamic file operations are implemented using the Linux native asynchronous I/O interface provided through `libaio`.

At server startup, an AIO context is initialized using `io_setup()`. Each connection contains the control structures necessary for submitting an asynchronous read operation.

Before starting a read, the server prepares an I/O control block containing the file descriptor, destination buffer, amount of data to read, and current offset inside the file.

The request is then submitted using `io_submit()`.

An `eventfd` is associated with the asynchronous operation using `io_set_eventfd()`. This gives the server a file descriptor that can be used as a notification mechanism when the operation completes and allows asynchronous file activity to be integrated with the event-driven architecture.

After completion, `io_getevents()` is used to obtain the result of the asynchronous operation. The connection updates its current file position and sends the newly available data to the client.

If the file has not been completely processed, another asynchronous operation is submitted. Otherwise, the connection can be closed.

This part of the implementation combines several Linux-specific mechanisms and was one of the main systems-programming aspects of the project.

## Non-Blocking Socket I/O

Accepted client sockets are explicitly configured in non-blocking mode.

When the server sends data using `send()`, it cannot assume that the entire buffer will always be accepted by the socket immediately. The implementation therefore keeps track of how much data has been transmitted and continues while progress can be made.

The cases represented by `EAGAIN` and `EWOULDBLOCK` are handled separately because they do not necessarily represent a permanent connection failure. They indicate that sending additional data would currently block.

Working with non-blocking sockets changes the way network applications have to be structured. Instead of assuming that every operation completes immediately, the server has to coordinate socket readiness, connection state, buffering, and file operations.

Together with `epoll`, this forms the basis of the event-driven model used throughout the project.

## Resource and Connection Management

The server manages network connections and file resources explicitly.

When a connection is finished, its socket is removed from the `epoll` instance and the associated file descriptors are closed.

The project also keeps track of file positions, response buffers, parser state, asynchronous I/O structures, and connection states inside each connection handler.

TCP communication is built directly on top of the Linux socket API, using operations such as `socket()`, `setsockopt()`, `bind()`, `listen()`, `accept()`, `recv()`, `send()`, and `shutdown()`.

This direct interaction with the operating system was an important part of the project, since it required managing the complete lifecycle of a connection instead of relying on an external web-server framework.

## Project Structure

- `aws.c` — main server implementation, event loop, connection handling, HTTP processing, static transfers, and asynchronous file I/O
- `aws.h` — connection structure, server constants, connection states, resource types, and function declarations
- `http-parser/` — HTTP parsing implementation used to extract information from incoming requests
- `utils/sock_util.c` / `utils/sock_util.h` — helper functions for TCP socket creation, connection management, and peer information
- `utils/w_epoll.h` — helper functions for working with `epoll`
- `utils/debug.h` / `utils/util.h` — debugging and utility definitions
- `Makefile` — project build configuration

## Build and Run

The project is designed for a **Linux environment** and is built using **GCC** and **GNU Make**.

Running `make` compiles the server, the HTTP parser, and the socket utilities and produces the `aws` executable.

The project links against `libaio`, so the Linux AIO library must be available on the system when the server is built.

The server listens on port `8888` and uses the current directory as its document root.

Resources under `/static/` are handled through the static transfer path using `sendfile()`, while resources under `/dynamic/` are handled through Linux asynchronous I/O.

## Technologies and Concepts

- C
- Linux
- TCP/IP
- HTTP
- BSD sockets
- Non-blocking sockets
- Event-driven programming
- `epoll`
- Linux AIO / `libaio`
- `eventfd`
- `sendfile`
- HTTP parsing
- File descriptors
- State machines
- Asynchronous file I/O
- Low-level network programming
- System calls
- Resource management
- GCC
- GNU Make
