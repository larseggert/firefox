#!/usr/bin/env python
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""
Helper library for creating a Signed Certificate Timestamp given the
details of a signing key, when to sign, and the certificate data to
sign. See RFC 6962.

When run with an output file-like object and a path to a file containing
a specification, creates an SCT from the given information and writes it
to the output object. The specification is as follows:

timestamp:<YYYYMMDD>
[key:<key specification>]
[tamper]
[leafIndex:<leaf index>]
certificate:
<certificate specification>

Where:
  [] indicates an optional field or component of a field
  <> indicates a required component of a field

By default, the "default" key is used (logs are essentially identified
by key). Other keys known to pykey can be specified.

The certificate specification must come last.
"""

import binascii
import calendar
import datetime
import hashlib
from io import StringIO
from struct import pack

import pycert
import pykey
from pyasn1.codec.der import encoder


class InvalidKeyError(Exception):
    """Helper exception to handle unknown key types."""

    def __init__(self, key):
        self.key = key

    def __str__(self):
        return f'Invalid key: "{str(self.key)}"'


class UnknownSignedEntryType(Exception):
    """Helper exception to handle unknown SignedEntry types."""

    def __init__(self, signedEntry):
        self.signedEntry = signedEntry

    def __str__(self):
        return f'Unknown SignedEntry type: "{str(self.signedEntry)}"'


class SignedEntry:
    """Base class for CT entries. Use PrecertEntry or
    X509Entry."""


class PrecertEntry(SignedEntry):
    """Precertificate entry type for SCT."""

    def __init__(self, tbsCertificate, issuerKey):
        self.tbsCertificate = tbsCertificate
        self.issuerKey = issuerKey


class X509Entry(SignedEntry):
    """x509 certificate entry type for SCT."""

    def __init__(self, certificate):
        self.certificate = certificate


class SCT:
    """SCT represents a Signed Certificate Timestamp."""

    def __init__(self, key, date, signedEntry, leafIndex=None):
        self.key = key
        self.timestamp = calendar.timegm(date.timetuple()) * 1000
        self.signedEntry = signedEntry
        self.tamper = False
        self.leafIndex = leafIndex

    def signAndEncode(self):
        """Returns a signed and encoded representation of the
        SCT as a string."""
        # The signature is over the following data:
        # sct_version (one 0 byte)
        # signature_type (one 0 byte)
        # timestamp (8 bytes, milliseconds since the epoch)
        # entry_type (two bytes (one 0 byte followed by one 0 byte for
        #             X509Entry or one 1 byte for PrecertEntry)
        # signed_entry (bytes of X509Entry or PrecertEntry)
        # extensions (2-byte-length-prefixed)
        # A X509Entry is:
        # certificate (3-byte-length-prefixed data)
        # A PrecertEntry is:
        # issuer_key_hash (32 bytes of SHA-256 hash of the issuing
        #                  public key, as DER-encoded SPKI)
        # tbs_certificate (3-byte-length-prefixed data)
        timestamp = pack("!Q", self.timestamp)

        if isinstance(self.signedEntry, X509Entry):
            len_prefix = pack("!L", len(self.signedEntry.certificate))[1:]
            entry_with_type = b"\0" + len_prefix + self.signedEntry.certificate
        elif isinstance(self.signedEntry, PrecertEntry):
            hasher = hashlib.sha256()
            hasher.update(
                encoder.encode(self.signedEntry.issuerKey.asSubjectPublicKeyInfo())
            )
            issuer_key_hash = hasher.digest()
            len_prefix = pack("!L", len(self.signedEntry.tbsCertificate))[1:]
            entry_with_type = (
                b"\1" + issuer_key_hash + len_prefix + self.signedEntry.tbsCertificate
            )
        else:
            raise UnknownSignedEntryType(self.signedEntry)
        extensions = []
        if self.leafIndex:
            # An extension consists of 1 byte to identify the extension type, 2
            # big-endian bytes for the length of the extension data, and then
            # the extension data.
            # The type of leaf_index is 0, and its data consists of 5 bytes.
            extensions = [b"\0\0\5" + self.leafIndex.to_bytes(5, byteorder="big")]
        extensionsLength = sum(map(len, extensions))
        extensionsEncoded = extensionsLength.to_bytes(2, byteorder="big") + b"".join(
            extensions
        )
        data = b"\0\0" + timestamp + b"\0" + entry_with_type + extensionsEncoded
        if isinstance(self.key, pykey.ECCKey):
            signatureByte = b"\3"
        elif isinstance(self.key, pykey.RSAKey):
            signatureByte = b"\1"
        else:
            raise InvalidKeyError(self.key)
        # sign returns a hex string like "'<hex bytes>'H", but we want
        # bytes here
        hexSignature = self.key.sign(data, pykey.HASH_SHA256)
        signature = bytearray(binascii.unhexlify(hexSignature[1:-2]))
        if self.tamper:
            signature[-1] = ~signature[-1] & 0xFF
        # The actual data returned is the following:
        # sct_version (one 0 byte)
        # id (32 bytes of SHA-256 hash of the signing key, as
        #     DER-encoded SPKI)
        # timestamp (8 bytes, milliseconds since the epoch)
        # extensions (2-byte-length-prefixed data)
        # hash (one 4 byte representing sha256)
        # signature (one byte - 1 for RSA and 3 for ECDSA)
        # signature (2-byte-length-prefixed data)
        hasher = hashlib.sha256()
        hasher.update(encoder.encode(self.key.asSubjectPublicKeyInfo()))
        key_id = hasher.digest()
        signature_len_prefix = pack("!H", len(signature))
        return (
            b"\0"
            + key_id
            + timestamp
            + extensionsEncoded
            + b"\4"
            + signatureByte
            + signature_len_prefix
            + signature
        )

    @staticmethod
    def fromSpecification(specStream):
        key = pykey.keyFromSpecification("default")
        certificateSpecification = StringIO()
        readingCertificateSpecification = False
        tamper = False
        leafIndex = None
        for line in specStream.readlines():
            lineStripped = line.strip()
            if readingCertificateSpecification:
                print(lineStripped, file=certificateSpecification)
            elif lineStripped == "certificate:":
                readingCertificateSpecification = True
            elif lineStripped.startswith("key:"):
                key = pykey.keyFromSpecification(lineStripped[len("key:") :])
            elif lineStripped.startswith("timestamp:"):
                timestamp = datetime.datetime.strptime(
                    lineStripped[len("timestamp:") :], "%Y%m%d"
                )
            elif lineStripped == "tamper":
                tamper = True
            elif lineStripped.startswith("leafIndex:"):
                leafIndex = int(lineStripped[len("leafIndex:") :])
            else:
                raise pycert.UnknownParameterTypeError(lineStripped)
        certificateSpecification.seek(0)
        certificate = pycert.Certificate(certificateSpecification).toDER()
        sct = SCT(key, timestamp, X509Entry(certificate))
        sct.tamper = tamper
        sct.leafIndex = leafIndex
        return sct


# The build harness will call this function with an output
# file-like object and a path to a file containing an SCT
# specification. This will read the specification and output
# the SCT as bytes.
def main(output, inputPath):
    with open(inputPath) as configStream:
        output.write(SCT.fromSpecification(configStream).signAndEncode())
