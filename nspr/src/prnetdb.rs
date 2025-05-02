/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::ffi;

use crate::{
    prio::{PRIPv6Addr, PRNetAddr},
    prtypes::{PRBool, PRIntn, PRStatus, PRUint16, PRUint32, PRUint64},
};

/*
 ********************************************************************
 *  Translate an Internet address to/from a character string
 ********************************************************************
 */
// NSPR_API(PRStatus) PR_StringToNetAddr(
//     const char *string, PRNetAddr *addr);

#[no_mangle]
pub extern "C" fn PR_StringToNetAddr(string: *const ffi::c_char, addr: *mut PRNetAddr) -> PRStatus {
    unimplemented!()
}
//
// NSPR_API(PRStatus) PR_NetAddrToString(
//     const PRNetAddr *addr, char *string, PRUint32 size);

#[no_mangle]
pub extern "C" fn PR_NetAddrToString(
    addr: *const PRNetAddr,
    string: *mut ffi::c_char,
    size: PRUint32,
) -> PRStatus {
    unimplemented!()
}

/*
 * Structures returned by network data base library.  All addresses are
 * supplied in host order, and returned in network order (suitable for
 * use in system calls).
 */
/*
 * Beware that WINSOCK.H defines h_addrtype and h_length as short.
 * Client code does direct struct copies of hostent to PRHostEnt and
 * hence the ifdef.
 */
// typedef struct PRHostEnt {
//     char *h_name;       /* official name of host */
//     char **h_aliases;   /* alias list */
// #ifdef WIN32
//     PRInt16 h_addrtype; /* host address type */
//     PRInt16 h_length;   /* length of address */
// #else
//     PRInt32 h_addrtype; /* host address type */
//     PRInt32 h_length;   /* length of address */
// #endif
//     char **h_addr_list; /* list of addresses from name server */
// } PRHostEnt;
pub type PRHostEnt = libc::hostent;

/* A safe size to use that will mostly work... */
pub const PR_NETDB_BUF_SIZE: ffi::c_int = 2048;
// #if (defined(AIX) && defined(_THREAD_SAFE))
// #define PR_NETDB_BUF_SIZE sizeof(struct protoent_data)
// #define PR_MIN_NETDB_BUF_SIZE PR_NETDB_BUF_SIZE
// #else
// /* PR_NETDB_BUF_SIZE is the recommended buffer size */
// #define PR_NETDB_BUF_SIZE 2048
// /* PR_MIN_NETDB_BUF_SIZE is the smallest buffer size that the API
//  * accepts (for backward compatibility). */
// #define PR_MIN_NETDB_BUF_SIZE 1024
// #endif

/***********************************************************************
 * FUNCTION:
 * DESCRIPTION: PR_GetHostByName()
 * Lookup a host by name.
 *
 * INPUTS:
 *  char *hostname      Character string defining the host name of interest
 *  char *buf           A scratch buffer for the runtime to return result.
 *                      This buffer is allocated by the caller.
 *  PRIntn bufsize      Number of bytes in 'buf'. A recommnded value to
 *                      use is PR_NETDB_BUF_SIZE.
 * OUTPUTS:
 *  PRHostEnt *hostentry
 *                      This structure is filled in by the runtime if
 *                      the function returns PR_SUCCESS. This structure
 *                      is allocated by the caller.
 * RETURN:
 *  PRStatus            PR_SUCCESS if the lookup succeeds. If it fails
 *                      the result will be PR_FAILURE and the reason
 *                      for the failure can be retrieved by PR_GetError().
 ********************************************************************* */
// NSPR_API(PRStatus) PR_GetHostByName(
//     const char *hostname, char *buf, PRIntn bufsize, PRHostEnt *hostentry);
#[no_mangle]
pub extern "C" fn PR_GetHostByName(
    hostname: *const ffi::c_char,
    buf: *mut ffi::c_char,
    bufsize: PRIntn,
    hostentry: *mut PRHostEnt,
) -> PRStatus {
    unimplemented!()
}

// /***********************************************************************
// ** FUNCTION:
// ** DESCRIPTION: PR_GetIPNodeByName()
// ** Lookup a host by name. Equivalent to getipnodebyname(AI_DEFAULT)
// ** of RFC 2553.
// **
// ** INPUTS:
// **  char *hostname      Character string defining the host name of interest
// **  PRUint16 af         Address family (either PR_AF_INET or PR_AF_INET6)
// **  PRIntn flags        Specifies the types of addresses that are searched
// **                      for and the types of addresses that are returned.
// **                      The only supported flag is PR_AI_DEFAULT.
// **  char *buf           A scratch buffer for the runtime to return result.
// **                      This buffer is allocated by the caller.
// **  PRIntn bufsize      Number of bytes in 'buf'. A recommnded value to
// **                      use is PR_NETDB_BUF_SIZE.
// ** OUTPUTS:
// **  PRHostEnt *hostentry
// **                      This structure is filled in by the runtime if
// **                      the function returns PR_SUCCESS. This structure
// **                      is allocated by the caller.
// ** RETURN:
// **  PRStatus            PR_SUCCESS if the lookup succeeds. If it fails
// **                      the result will be PR_FAILURE and the reason
// **                      for the failure can be retrieved by PR_GetError().
// ***********************************************************************/
//
//
// #define PR_AI_ALL         0x08
// #define PR_AI_V4MAPPED    0x10
// #define PR_AI_ADDRCONFIG  0x20
pub const PR_AI_ADDRCONFIG: ffi::c_int = 0x20;
// #define PR_AI_NOCANONNAME 0x8000
pub const PR_AI_NOCANONNAME: ffi::c_int = 0x8000;
// #define PR_AI_DEFAULT     (PR_AI_V4MAPPED | PR_AI_ADDRCONFIG)
//
// NSPR_API(PRStatus) PR_GetIPNodeByName(
//     const char *hostname,
//     PRUint16 af,
//     PRIntn flags,
//     char *buf,
//     PRIntn bufsize,
//     PRHostEnt *hostentry);
//
// /***********************************************************************
// ** FUNCTION:
// ** DESCRIPTION: PR_GetHostByAddr()
// ** Lookup a host entry by its network address.
// **
// ** INPUTS:
// **  char *hostaddr      IP address of host in question
// **  char *buf           A scratch buffer for the runtime to return result.
// **                      This buffer is allocated by the caller.
// **  PRIntn bufsize      Number of bytes in 'buf'. A recommnded value to
// **                      use is PR_NETDB_BUF_SIZE.
// ** OUTPUTS:
// **  PRHostEnt *hostentry
// **                      This structure is filled in by the runtime if
// **                      the function returns PR_SUCCESS. This structure
// **                      is allocated by the caller.
// ** RETURN:
// **  PRStatus            PR_SUCCESS if the lookup succeeds. If it fails
// **                      the result will be PR_FAILURE and the reason
// **                      for the failure can be retrieved by PR_GetError().
// ***********************************************************************/
// NSPR_API(PRStatus) PR_GetHostByAddr(
//     const PRNetAddr *hostaddr, char *buf, PRIntn bufsize, PRHostEnt *hostentry);

/***********************************************************************
 * FUNCTION:    PR_EnumerateHostEnt()
 * DESCRIPTION:
 *  A stateless enumerator over a PRHostEnt structure acquired from
 *  PR_GetHostByName() PR_GetHostByAddr() to evaluate the possible
 *  network addresses.
 *
 * INPUTS:
 *  PRIntn  enumIndex   Index of the enumeration. The enumeration starts
 *                      and ends with a value of zero.
 *
 *  PRHostEnt *hostEnt  A pointer to a host entry struct that was
 *                      previously returned by PR_GetHostByName() or
 *                      PR_GetHostByAddr().
 *
 *  PRUint16 port       The port number to be assigned as part of the
 *                      PRNetAddr.
 *
 * OUTPUTS:
 *  PRNetAddr *address  A pointer to an address structure that will be
 *                      filled in by the call to the enumeration if the
 *                      result of the call is greater than zero.
 *
 * RETURN:
 *  PRIntn              The value that should be used for the next call
 *                      of the enumerator ('enumIndex'). The enumeration
 *                      is ended if this value is returned zero.
 *                      If a value of -1 is returned, the enumeration
 *                      has failed. The reason for the failure can be
 *                      retrieved by calling PR_GetError().
 ********************************************************************* */
// NSPR_API(PRIntn) PR_EnumerateHostEnt(
//     PRIntn enumIndex, const PRHostEnt *hostEnt, PRUint16 port, PRNetAddr *address);
#[no_mangle]
pub extern "C" fn PR_EnumerateHostEnt(
    enumIndex: PRIntn,
    hostEnt: *const PRHostEnt,
    port: PRUint16,
    address: *mut PRNetAddr,
) -> PRIntn {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION: PR_InitializeNetAddr(),
 * DESCRIPTION:
 *  Initialize the fields of a PRNetAddr, assigning well known values as
 *  appropriate.
 *
 * INPUTS
 *  PRNetAddrValue val  The value to be assigned to the IP Address portion
 *                      of the network address. This can only specify the
 *                      special well known values that are equivalent to
 *                      INADDR_ANY and INADDR_LOOPBACK.
 *
 *  PRUint16 port       The port number to be assigned in the structure.
 *
 * OUTPUTS:
 *  PRNetAddr *addr     The address to be manipulated.
 *
 * RETURN:
 *  PRStatus            To indicate success or failure. If the latter, the
 *                      reason for the failure can be retrieved by calling
 *                      PR_GetError();
 ********************************************************************* */
#[repr(C)]
pub enum PRNetAddrValue {
    PR_IpAddrNull,     /* do NOT overwrite the IP address */
    PR_IpAddrAny,      /* assign logical INADDR_ANY to IP address */
    PR_IpAddrLoopback, /* assign logical INADDR_LOOPBACK */
    PR_IpAddrV4Mapped, /* IPv4 mapped address */
}

// NSPR_API(PRStatus) PR_InitializeNetAddr(
//     PRNetAddrValue val, PRUint16 port, PRNetAddr *addr);
#[no_mangle]
pub extern "C" fn PR_InitializeNetAddr(
    val: PRNetAddrValue,
    port: PRUint16,
    addr: *mut PRNetAddr,
) -> PRStatus {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION: PR_SetNetAddr(),
 * DESCRIPTION:
 *  Set the fields of a PRNetAddr, assigning well known values as
 *  appropriate. This function is similar to PR_InitializeNetAddr
 *  but differs in that the address family is specified.
 *
 * INPUTS
 *  PRNetAddrValue val  The value to be assigned to the IP Address portion
 *                      of the network address. This can only specify the
 *                      special well known values that are equivalent to
 *                      INADDR_ANY and INADDR_LOOPBACK.
 *
 *  PRUint16 af         The address family (either PR_AF_INET or PR_AF_INET6)
 *
 *  PRUint16 port       The port number to be assigned in the structure.
 *
 * OUTPUTS:
 *  PRNetAddr *addr     The address to be manipulated.
 *
 * RETURN:
 *  PRStatus            To indicate success or failure. If the latter, the
 *                      reason for the failure can be retrieved by calling
 *                      PR_GetError();
 ********************************************************************* */
// NSPR_API(PRStatus) PR_SetNetAddr(
//     PRNetAddrValue val, PRUint16 af, PRUint16 port, PRNetAddr *addr);
#[no_mangle]
pub extern "C" fn PR_SetNetAddr(
    val: PRNetAddrValue,
    af: PRUint16,
    port: PRUint16,
    addr: *mut PRNetAddr,
) -> PRStatus {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:
 * DESCRIPTION: PR_IsNetAddrType()
 * Determine if the network address is of the specified type.
 *
 * INPUTS:
 *  const PRNetAddr *addr   A network address.
 *  PRNetAddrValue          The type of network address
 *
 * RETURN:
 *  PRBool                  PR_TRUE if the network address is of the
 *                          specified type, else PR_FALSE.
 ********************************************************************* */
// NSPR_API(PRBool) PR_IsNetAddrType(const PRNetAddr *addr, PRNetAddrValue val);
#[no_mangle]
pub extern "C" fn PR_IsNetAddrType(addr: *const PRNetAddr, val: PRNetAddrValue) -> PRBool {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:
 * DESCRIPTION: PR_ConvertIPv4AddrToIPv6()
 * Convert an IPv4 addr to an (IPv4-mapped) IPv6 addr
 *
 * INPUTS:
 *  PRUint32    v4addr      IPv4 address
 *
 * OUTPUTS:
 *  PRIPv6Addr *v6addr      The converted IPv6 address
 *
 * RETURN:
 *  void
 *
 ********************************************************************* */
// NSPR_API(void) PR_ConvertIPv4AddrToIPv6(PRUint32 v4addr, PRIPv6Addr *v6addr);

#[no_mangle]
pub extern "C" fn PR_ConvertIPv4AddrToIPv6(v4addr: PRUint32, v6addr: *mut PRIPv6Addr) {
    unimplemented!()
}

/***********************************************************************
 * MACRO:
 * DESCRIPTION: PR_NetAddrFamily()
 * Get the 'family' field of a PRNetAddr union.
 *
 * INPUTS:
 *  const PRNetAddr *addr   A network address.
 *
 * RETURN:
 *  PRUint16                The 'family' field of 'addr'.
 ********************************************************************* */
// #define PR_NetAddrFamily(addr) ((addr)->raw.family)
#[no_mangle]
pub const extern "C" fn PR_NetAddrFamily(addr: *const PRNetAddr) -> PRUint16 {
    unsafe { (*addr).raw.family }
}

// /***********************************************************************
// ** MACRO:
// ** DESCRIPTION: PR_NetAddrInetPort()
// ** Get the 'port' field of a PRNetAddr union.
// **
// ** INPUTS:
// **  const PRNetAddr *addr   A network address.
// **
// ** RETURN:
// **  PRUint16                The 'port' field of 'addr'.
// ***********************************************************************/
// #define PR_NetAddrInetPort(addr) \
//     ((addr)->raw.family == PR_AF_INET6 ? (addr)->ipv6.port : (addr)->inet.port)
//
// /***********************************************************************
// ** FUNCTION:
// ** DESCRIPTION: PR_GetProtoByName()
// ** Lookup a protocol entry based on protocol's name
// **
// ** INPUTS:
// **  char *protocolname  Character string of the protocol's name.
// **  char *buf           A scratch buffer for the runtime to return result.
// **                      This buffer is allocated by the caller.
// **  PRIntn bufsize      Number of bytes in 'buf'. A recommnded value to
// **                      use is PR_NETDB_BUF_SIZE.
// ** OUTPUTS:
// **  PRHostEnt *PRProtoEnt
// **                      This structure is filled in by the runtime if
// **                      the function returns PR_SUCCESS. This structure
// **                      is allocated by the caller.
// ** RETURN:
// **  PRStatus            PR_SUCCESS if the lookup succeeds. If it fails
// **                      the result will be PR_FAILURE and the reason
// **                      for the failure can be retrieved by PR_GetError().
// ***********************************************************************/
//
// typedef struct PRProtoEnt {
//     char *p_name;       /* official protocol name */
//     char **p_aliases;   /* alias list */
// #ifdef WIN32
//     PRInt16 p_num;      /* protocol # */
// #else
//     PRInt32 p_num;      /* protocol # */
// #endif
// } PRProtoEnt;
//
// NSPR_API(PRStatus) PR_GetProtoByName(
//     const char* protocolname, char* buffer, PRInt32 bufsize, PRProtoEnt* result);
//
// /***********************************************************************
// ** FUNCTION:
// ** DESCRIPTION: PR_GetProtoByNumber()
// ** Lookup a protocol entry based on protocol's number
// **
// ** INPUTS:
// **  PRInt32 protocolnumber
// **                      Number assigned to the protocol.
// **  char *buf           A scratch buffer for the runtime to return result.
// **                      This buffer is allocated by the caller.
// **  PRIntn bufsize      Number of bytes in 'buf'. A recommnded value to
// **                      use is PR_NETDB_BUF_SIZE.
// ** OUTPUTS:
// **  PRHostEnt *PRProtoEnt
// **                      This structure is filled in by the runtime if
// **                      the function returns PR_SUCCESS. This structure
// **                      is allocated by the caller.
// ** RETURN:
// **  PRStatus            PR_SUCCESS if the lookup succeeds. If it fails
// **                      the result will be PR_FAILURE and the reason
// **                      for the failure can be retrieved by PR_GetError().
// ***********************************************************************/
// NSPR_API(PRStatus) PR_GetProtoByNumber(
//     PRInt32 protocolnumber, char* buffer, PRInt32 bufsize, PRProtoEnt* result);

/***********************************************************************
 * FUNCTION:
 * DESCRIPTION: PR_GetAddrInfoByName()
 *  Look up a host by name. Equivalent to getaddrinfo(host, NULL, ...) of
 *  RFC 3493.
 *
 * INPUTS:
 *  char *hostname      Character string defining the host name of interest
 *  PRUint16 af         May be PR_AF_UNSPEC or PR_AF_INET.
 *  PRIntn flags        May be either PR_AI_ADDRCONFIG or
 *                      PR_AI_ADDRCONFIG | PR_AI_NOCANONNAME. Include
 *                      PR_AI_NOCANONNAME to suppress the determination of
 *                      the canonical name corresponding to hostname.
 * RETURN:
 *  PRAddrInfo*         Handle to a data structure containing the results
 *                      of the host lookup. Use PR_EnumerateAddrInfo to
 *                      inspect the PRNetAddr values stored in this object.
 *                      When no longer needed, this handle must be destroyed
 *                      with a call to PR_FreeAddrInfo.  If a lookup error
 *                      occurs, then NULL will be returned.
 ********************************************************************* */
// typedef struct PRAddrInfo PRAddrInfo;
#[repr(transparent)]
pub struct PRAddrInfo(libc::addrinfo);

// NSPR_API(PRAddrInfo*) PR_GetAddrInfoByName(
//     const char *hostname, PRUint16 af, PRIntn flags);
#[no_mangle]
pub extern "C" fn PR_GetAddrInfoByName(
    hostname: *const ffi::c_char,
    af: PRUint16,
    flags: PRIntn,
) -> *mut PRAddrInfo {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:
 * DESCRIPTION: PR_FreeAddrInfo()
 *  Destroy the PRAddrInfo handle allocated by PR_GetAddrInfoByName().
 *
 * INPUTS:
 *  PRAddrInfo *addrInfo
 *                      The handle resulting from a successful call to
 *                      PR_GetAddrInfoByName().
 * RETURN:
 *  void
 ********************************************************************* */
// NSPR_API(void) PR_FreeAddrInfo(PRAddrInfo *addrInfo);
#[no_mangle]
pub extern "C" fn PR_FreeAddrInfo(addrInfo: *mut PRAddrInfo) {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:
 * DESCRIPTION: PR_EnumerateAddrInfo()
 *  A stateless enumerator over a PRAddrInfo handle acquired from
 *  PR_GetAddrInfoByName() to inspect the possible network addresses.
 *
 * INPUTS:
 *  void *enumPtr       Index pointer of the enumeration. The enumeration
 *                      starts and ends with a value of NULL.
 *  const PRAddrInfo *addrInfo
 *                      The PRAddrInfo handle returned by a successful
 *                      call to PR_GetAddrInfoByName().
 *  PRUint16 port       The port number to be assigned as part of the
 *                      PRNetAddr.
 * OUTPUTS:
 *  PRNetAddr *result   A pointer to an address structure that will be
 *                      filled in by the call to the enumeration if the
 *                      result of the call is not NULL.
 * RETURN:
 *  void*               The value that should be used for the next call
 *                      of the enumerator ('enumPtr'). The enumeration
 *                      is ended if this value is NULL.
 ********************************************************************* */
// NSPR_API(void *) PR_EnumerateAddrInfo(
//     void *enumPtr, const PRAddrInfo *addrInfo, PRUint16 port, PRNetAddr *result);
#[no_mangle]
pub extern "C" fn PR_EnumerateAddrInfo(
    enumPtr: *mut ffi::c_void,
    addrInfo: *const PRAddrInfo,
    port: PRUint16,
    result: *mut PRNetAddr,
) -> *mut ffi::c_void {
    unimplemented!()
}

// NSPR_API(PRStatus) PR_GetPrefLoopbackAddrInfo(PRNetAddr *result,
//                                               PRUint16 port);

/***********************************************************************
 * FUNCTION:
 * DESCRIPTION: PR_GetCanonNameFromAddrInfo()
 *  Extracts the canonical name of the hostname passed to
 *  PR_GetAddrInfoByName().
 *
 * INPUTS:
 *  const PRAddrInfo *addrInfo
 *                      The PRAddrInfo handle returned by a successful
 *                      call to PR_GetAddrInfoByName().
 * RETURN:
 *  const char *        A const pointer to the canonical hostname stored
 *                      in the given PRAddrInfo handle. This pointer is
 *                      invalidated once the PRAddrInfo handle is destroyed
 *                      by a call to PR_FreeAddrInfo().
 ********************************************************************* */
// NSPR_API(const char *) PR_GetCanonNameFromAddrInfo(
//     const PRAddrInfo *addrInfo);
#[no_mangle]
pub extern "C" fn PR_GetCanonNameFromAddrInfo(addrInfo: *const PRAddrInfo) -> *const ffi::c_char {
    unimplemented!()
}

/***********************************************************************
 * FUNCTIONS: PR_ntohs, PR_ntohl, PR_ntohll, PR_htons, PR_htonl, PR_htonll
 *
 * DESCRIPTION: API entries for the common byte ordering routines.
 *
 *      PR_ntohs        16 bit conversion from network to host
 *      PR_ntohl        32 bit conversion from network to host
 *      PR_ntohll       64 bit conversion from network to host
 *      PR_htons        16 bit conversion from host to network
 *      PR_htonl        32 bit conversion from host to network
 *      PR_ntonll       64 bit conversion from host to network
 *
 ********************************************************************* */
// NSPR_API(PRUint16) PR_ntohs(PRUint16);
#[no_mangle]
pub const extern "C" fn PR_ntohs(value: PRUint16) -> PRUint16 {
    PRUint16::from_be(value)
}
// NSPR_API(PRUint32) PR_ntohl(PRUint32);
#[no_mangle]
pub const extern "C" fn PR_ntohl(value: PRUint32) -> PRUint32 {
    PRUint32::from_be(value)
}
// NSPR_API(PRUint64) PR_ntohll(PRUint64);
// NSPR_API(PRUint16) PR_htons(PRUint16);
#[no_mangle]
pub const extern "C" fn PR_htons(value: PRUint16) -> PRUint16 {
    PRUint16::to_be(value)
}
// NSPR_API(PRUint32) PR_htonl(PRUint32);
#[no_mangle]
pub const extern "C" fn PR_htonl(value: PRUint32) -> PRUint32 {
    PRUint32::to_be(value)
}

// NSPR_API(PRUint64) PR_htonll(PRUint64);
#[no_mangle]
pub const extern "C" fn PR_htonll(value: PRUint64) -> PRUint64 {
    PRUint64::to_be(value)
}
