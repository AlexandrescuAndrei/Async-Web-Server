# Asynchronous HTTP Server in C

An asynchronous HTTP server implemented in **C** using Linux low-level I/O and networking APIs.

The project explores event-driven server architecture, non-blocking TCP sockets, I/O multiplexing with `epoll`, asynchronous file operations using Linux AIO, and efficient static file transfer using `sendfile`.

---

## Overview

The server listens for incoming HTTP connections and serves files requested by clients.

Instead of assigning a dedicated thread or blocking process to every connection, the server uses an **event-driven architecture** based on Linux `epoll`.

Client sockets are configured as non-blocking, allowing multiple connections and I/O events to be managed through a single event loop.

HTTP requests are parsed to determine the requested resource, after which the server chooses an appropriate file-transfer strategy.

---

## Key Features

- TCP server implemented using Linux sockets
- Non-blocking client connections
- Event-driven architecture using `epoll`
- HTTP request parsing
- Per-connection state machine
- HTTP `200 OK` responses
- HTTP `404 Not Found` handling
- Static file transfer using `sendfile`
- Asynchronous file reads using Linux AIO
- Completion notifications using `eventfd`
- Explicit connection and resource management
- Incremental buffered socket writes

---

## Server Architecture

The server initializes a TCP listening socket and binds it to port `8888`.

Incoming connections are accepted and configured in non-blocking mode.

Each connection is represented by a dedicated structure containing:

- Client socket descriptor
- File descriptor
- HTTP parser state
- Receive and send buffers
- Requested path
- File size and current position
- Linux AIO control structures
- `eventfd` descriptor
- Current connection state

The listening socket and connection-related events are monitored through an `epoll` instance.

The main event loop waits for events and dispatches them to the appropriate connection logic.

---

## Event-Driven I/O with epoll

The project uses Linux `epoll` to efficiently monitor file descriptors for I/O events.

Instead of continuously polling every connection or blocking while waiting for data, the server waits for the kernel to report descriptors that are ready for processing.

New client sockets are configured using:

```text
O_NONBLOCK
```

and registered with the `epoll` instance.

This architecture allows the server to manage multiple I/O operations through a centralized event loop.

---

## Connection State Machine

Each client connection progresses through a state machine describing its current stage.

States include:

```text
STATE_INITIAL
STATE_RECEIVING_DATA
STATE_REQUEST_RECEIVED
STATE_SENDING_HEADER
STATE_HEADER_SENT
STATE_ASYNC_ONGOING
STATE_SENDING_DATA
STATE_DATA_SENT
STATE_SENDING_404
STATE_404_SENT
STATE_CONNECTION_CLOSED
```

Separating connection behavior into explicit states makes it possible to coordinate request parsing, file operations, asynchronous I/O completion, and response transmission without relying on blocking control flow.

---

## HTTP Request Processing

Incoming data is received from the client socket and passed to an HTTP parser.

A path callback extracts the requested resource from the HTTP request and stores it in the connection structure.

The server distinguishes between two resource categories:

```text
/static/
/dynamic/
```

If the requested file cannot be opened, the server prepares and sends an HTTP:

```text
404 Not Found
```

response.

For valid files, an HTTP `200 OK` response header is generated with the appropriate content length.

---

## Static File Transfer

Static resources are transferred using the Linux `sendfile` system call.

`sendfile` provides an efficient mechanism for transferring file contents directly to a socket without requiring the application to manually copy every chunk through a user-space buffer.

This path is used for requests targeting the `/static/` resource directory.

---

## Asynchronous File I/O

Dynamic resources are handled using Linux asynchronous I/O through `libaio`.

The server initializes an asynchronous I/O context using:

```text
io_setup
```

and submits file read operations using:

```text
io_submit
```

Completed operations are retrieved through:

```text
io_getevents
```

This allows file reads to be coordinated without using a traditional blocking `read` operation for every chunk.

---

## eventfd Integration

Asynchronous file operations are associated with an `eventfd`.

The event descriptor provides a mechanism for notifying the event-driven system when an asynchronous operation completes.

This allows asynchronous file I/O completion to participate in the same general event-processing architecture used by the server.

---

## Buffered Socket Transmission

Data read asynchronously from files is stored in a connection-specific send buffer.

The server then attempts to transmit as much data as possible through the non-blocking socket.

The implementation explicitly handles:

```text
EAGAIN
EWOULDBLOCK
```

which indicate that the socket currently cannot accept additional data without blocking.

---

## Technologies and Concepts

- C
- Linux
- TCP/IP
- HTTP
- BSD Sockets API
- Non-blocking I/O
- epoll
- Linux AIO / libaio
- eventfd
- sendfile
- File descriptors
- HTTP parsing
- State machines
- Event-driven programming
- System calls
- GNU Make

---

## Build

The project uses GCC and GNU Make.

The server is linked against `libaio`.

```bash
make
```

The resulting executable is:

```text
aws
```

---

## Technical Focus

The primary focus of the project is understanding how asynchronous and event-driven servers can be implemented using low-level Linux primitives.

Rather than relying on a high-level networking framework, the implementation directly manages sockets, file descriptors, connection states, HTTP parsing, asynchronous disk operations, and resource cleanup.

The project demonstrates how mechanisms such as `epoll`, non-blocking sockets, `eventfd`, Linux AIO, and `sendfile` can be combined to construct an asynchronous HTTP server.
