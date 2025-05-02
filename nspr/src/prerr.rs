/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::prerror::PRErrorCode;

/// Memory allocation attempt failed
pub const PR_OUT_OF_MEMORY_ERROR: PRErrorCode = -6000;

/// Invalid file descriptor
pub const PR_BAD_DESCRIPTOR_ERROR: PRErrorCode = -5999;

/* The operation would have blocked */
pub const PR_WOULD_BLOCK_ERROR: PRErrorCode = -5998;

/// Invalid memory address argument
pub const PR_ACCESS_FAULT_ERROR: PRErrorCode = -5997;

/// Invalid function for file type
pub const PR_INVALID_METHOD_ERROR: PRErrorCode = -5996;

/// Some unknown error has occurred
pub const PR_UNKNOWN_ERROR: PRErrorCode = -5994;

/// Operation interrupted by another thread
pub const PR_PENDING_INTERRUPT_ERROR: PRErrorCode = -5993;

/// function not implemented
pub const PR_NOT_IMPLEMENTED_ERROR: PRErrorCode = -5992;

/// I/O function error
pub const PR_IO_ERROR: PRErrorCode = -5991;

/// I/O operation timed out
pub const PR_IO_TIMEOUT_ERROR: PRErrorCode = -5990;

/// Invalid function argument
pub const PR_INVALID_ARGUMENT_ERROR: PRErrorCode = -5987;

/// Network address not available (in use?)
pub const PR_ADDRESS_NOT_AVAILABLE_ERROR: PRErrorCode = -5986;

/// Network address type not supported
pub const PR_ADDRESS_NOT_SUPPORTED_ERROR: PRErrorCode = -5985;

/// Already connected
pub const PR_IS_CONNECTED_ERROR: PRErrorCode = -5984;

/// Network address is invalid
pub const PR_BAD_ADDRESS_ERROR: PRErrorCode = -5983;

/// Local Network address is in use
pub const PR_ADDRESS_IN_USE_ERROR: PRErrorCode = -5982;

/// Connection refused by peer
pub const PR_CONNECT_REFUSED_ERROR: PRErrorCode = -5981;

/// Network address is presently unreachable
pub const PR_NETWORK_UNREACHABLE_ERROR: PRErrorCode = -5980;

/// Connection attempt timed out
pub const PR_CONNECT_TIMEOUT_ERROR: PRErrorCode = -5979;

/// Network file descriptor is not connected
pub const PR_NOT_CONNECTED_ERROR: PRErrorCode = -5978;

/// Failure to load dynamic library
pub const PR_LOAD_LIBRARY_ERROR: PRErrorCode = -5977;

/// Insufficient system resources
pub const PR_INSUFFICIENT_RESOURCES_ERROR: PRErrorCode = -5974;

/// Attempt to access a TPD key that is out of range
pub const PR_TPD_RANGE_ERROR: PRErrorCode = -5972;

/// Process open FD table is full
pub const PR_PROC_DESC_TABLE_FULL_ERROR: PRErrorCode = -5971;

/// System open FD table is full
pub const PR_SYS_DESC_TABLE_FULL_ERROR: PRErrorCode = -5970;

/// Network operation attempted on non-network file descriptor
pub const PR_NOT_SOCKET_ERROR: PRErrorCode = -5969;

/// TCP-specific function attempted on a non-TCP file descriptor
pub const PR_NOT_TCP_SOCKET_ERROR: PRErrorCode = -5968;

/// TCP file descriptor is already bound
pub const PR_SOCKET_ADDRESS_IS_BOUND_ERROR: PRErrorCode = -5967;

/// Access Denied
pub const PR_NO_ACCESS_RIGHTS_ERROR: PRErrorCode = -5966;

/// The requested operation is not supported by the platform
pub const PR_OPERATION_NOT_SUPPORTED_ERROR: PRErrorCode = -5965;

/// The host operating system does not support the protocol requested
pub const PR_PROTOCOL_NOT_SUPPORTED_ERROR: PRErrorCode = -5964;

/// Access to the remote file has been severed
pub const PR_REMOTE_FILE_ERROR: PRErrorCode = -5963;

/// The value requested is too large to be stored in the data buffer provided
pub const PR_BUFFER_OVERFLOW_ERROR: PRErrorCode = -5962;

/// TCP connection reset by peer
pub const PR_CONNECT_RESET_ERROR: PRErrorCode = -5961;

/// The operation would have deadlocked
pub const PR_DEADLOCK_ERROR: PRErrorCode = -5959;

/// The file is already locked
pub const PR_FILE_IS_LOCKED_ERROR: PRErrorCode = -5958;

/// Write would result in file larger than the system allows
pub const PR_FILE_TOO_BIG_ERROR: PRErrorCode = -5957;

/// The device for storing the file is full
pub const PR_NO_DEVICE_SPACE_ERROR: PRErrorCode = -5956;

/// Cannot perform a normal file operation on a directory
pub const PR_IS_DIRECTORY_ERROR: PRErrorCode = -5953;

/// Symbolic link loop
pub const PR_LOOP_ERROR: PRErrorCode = -5952;

/// File name is too long
pub const PR_NAME_TOO_LONG_ERROR: PRErrorCode = -5951;

/// File not found
pub const PR_FILE_NOT_FOUND_ERROR: PRErrorCode = -5950;

/// Cannot perform directory operation on a normal file
pub const PR_NOT_DIRECTORY_ERROR: PRErrorCode = -5949;

/// Cannot write to a read-only file system
pub const PR_READ_ONLY_FILESYSTEM_ERROR: PRErrorCode = -5948;

/// Cannot delete a directory that is not empty
pub const PR_DIRECTORY_NOT_EMPTY_ERROR: PRErrorCode = -5947;

/// Cannot delete or rename a file object while the file system is busy
pub const PR_FILESYSTEM_MOUNTED_ERROR: PRErrorCode = -5946;

/// Cannot rename a file to a file system on another device
pub const PR_NOT_SAME_DEVICE_ERROR: PRErrorCode = -5945;

/// Cannot create or rename a filename that already exists
pub const PR_FILE_EXISTS_ERROR: PRErrorCode = -5943;

/// Directory is full.  No additional filenames may be added
pub const PR_MAX_DIRECTORY_ENTRIES_ERROR: PRErrorCode = -5942;

/// No more entries in the directory
pub const PR_NO_MORE_FILES_ERROR: PRErrorCode = -5939;

/// Encountered end of file
pub const PR_END_OF_FILE_ERROR: PRErrorCode = -5938;

/// Operation is still in progress (probably a non-blocking connect)
pub const PR_IN_PROGRESS_ERROR: PRErrorCode = -5934;

/// Operation has already been initiated (probably a non-blocking connect)
pub const PR_ALREADY_INITIATED_ERROR: PRErrorCode = -5933;

/// Object state improper for request
pub const PR_INVALID_STATE_ERROR: PRErrorCode = -5931;

/// Socket shutdown
pub const PR_SOCKET_SHUTDOWN_ERROR: PRErrorCode = -5929;

/// Connection aborted
pub const PR_CONNECT_ABORTED_ERROR: PRErrorCode = -5928;

/// Host is unreachable
pub const PR_HOST_UNREACHABLE_ERROR: PRErrorCode = -5927;

/// Placeholder for the end of the list
pub const PR_MAX_ERROR: PRErrorCode = -5924;
