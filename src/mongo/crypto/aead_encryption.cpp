/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/crypto/aead_encryption.h"

#include "mongo/base/data_view.h"
#include "mongo/crypto/sha512_block.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/util/secure_compare_memory.h"

namespace mongo {
namespace crypto {

namespace {
constexpr size_t kIVSize = 16;

// AssociatedData can be 2^24 bytes but since there needs to be room for the ciphertext in the
// object, a value of 1<<16 was decided to cap the maximum size of AssociatedData.
constexpr int kMaxAssociatedDataLength = 1 << 16;

size_t aesCBCCipherOutputLength(size_t plainTextLen) {
    return aesBlockSize * (1 + plainTextLen / aesBlockSize);
}

std::pair<size_t, size_t> aesCBCExpectedPlaintextLen(size_t cipherTextLength) {
    return {cipherTextLength - aesCBCIVSize - aesBlockSize, cipherTextLength - aesCBCIVSize};
}

void aeadGenerateIV(const SymmetricKey* key, DataRange buffer) {
    if (buffer.length() < aesCBCIVSize) {
        fassert(51235, "IV buffer is too small for selected mode");
    }

    auto status = engineRandBytes(buffer.slice(aesCBCIVSize));
    if (!status.isOK()) {
        fassert(51236, status);
    }
}

StatusWith<std::size_t> _aesEncrypt(const SymmetricKey& key,
                                    ConstDataRange in,
                                    DataRange outRange,
                                    bool ivProvided) try {
    if (!ivProvided) {
        aeadGenerateIV(&key, outRange);
    }

    DataRangeCursor out(outRange);
    DataRange iv = out.sliceAndAdvance(aesCBCIVSize);

    auto encryptor = uassertStatusOK(SymmetricEncryptor::create(key, aesMode::cbc, iv));

    const auto updateLen = uassertStatusOK(encryptor->update(in, out));
    out.advance(updateLen);

    const auto finalLen = uassertStatusOK(encryptor->finalize(out));
    out.advance(finalLen);

    // Some cipher modes, such as GCM, will know in advance exactly how large their ciphertexts will
    // be. Others, like CBC, will have an upper bound. When this is true, we must allocate enough
    // memory to store the worst case. We must then set the actual size of the ciphertext so that
    // the buffer it has been written to may be serialized.
    const auto len = updateLen + finalLen;

    // Check the returned length, including block size padding
    if (len != aesCBCCipherOutputLength(in.length())) {
        return {ErrorCodes::BadValue,
                str::stream() << "Encrypt error, expected cipher text of length "
                              << aesCBCCipherOutputLength(in.length()) << " but found " << len};
    }

    return aesCBCIVSize + len;
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

StatusWith<std::size_t> _aesDecrypt(const SymmetricKey& key,
                                    ConstDataRange inRange,
                                    DataRange outRange) try {
    // Check the plaintext buffer can fit the product of decryption
    auto [lowerBound, upperBound] = aesCBCExpectedPlaintextLen(inRange.length());
    if (upperBound > outRange.length()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cleartext buffer of size " << outRange.length()
                              << " too small for output which can be as large as " << upperBound
                              << "]"};
    }


    ConstDataRangeCursor in(inRange);
    auto iv = in.sliceAndAdvance(aesCBCIVSize);

    auto decryptor = uassertStatusOK(SymmetricDecryptor::create(key, aesMode::cbc, iv));

    DataRangeCursor out(outRange);
    const auto updateLen = uassertStatusOK(decryptor->update(in, out));
    out.advance(updateLen);

    const auto finalLen = uassertStatusOK(decryptor->finalize(out));
    out.advance(finalLen);

    auto outputLen = updateLen + finalLen;

    // Check the returned length, excluding headers block padding
    if ((outputLen < lowerBound) || (outputLen > upperBound)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Decrypt error, expected clear text length in interval"
                              << "[" << lowerBound << "," << upperBound << "]"
                              << "but found " << outputLen};
    }

    /* Check that padding was removed.
     *
     * PKCS7 padding guarantees that the encrypted payload has
     * between 1 and blocksize bytes of padding which should be
     * removed during the decrypt process.
     *
     * If resultLen is the same as the payload len,
     * that means no padding was removed.
     *
     * macOS CommonCrypto will return such payloads when either the
     * key or ciphertext are corrupted and its unable to find any
     * expected padding.  It fails open by returning whatever it can.
     */
    if (outputLen >= in.length()) {
        return {ErrorCodes::BadValue,
                "Decrypt error, plaintext is as large or larger than "
                "the ciphertext. This usually indicates an invalid key."};
    }

    return outputLen;
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

}  // namespace

size_t aeadCipherOutputLength(size_t plainTextLen) {
    // To calculate the size of the byte, we divide by the byte size and add 2 for padding
    // (1 for the attached IV, and 1 for the extra padding). The algorithm will add padding even
    // if the len is a multiple of the byte size, so if the len divides cleanly it will be
    // 32 bytes longer than the original, which is 16 bytes as padding and 16 bytes for the
    // IV. For things that don't divide cleanly, the cast takes care of floor dividing so it will
    // be 0 < x < 16 bytes added for padding and 16 bytes added for the IV.
    size_t aesOutLen = aesBlockSize * (plainTextLen / aesBlockSize + 2);
    return aesOutLen + kHmacOutSize;
}

Status aeadEncryptLocalKMS(const SymmetricKey& key, ConstDataRange in, DataRange out) {
    if (key.getKeySize() != kFieldLevelEncryptionKeySize) {
        return Status(ErrorCodes::BadValue,
                      "AEAD encryption key is the incorrect length. "
                      "Must be 96 bytes.");
    }

    // According to the rfc on AES encryption, the associatedDataLength is defined as the
    // number of bits in associatedData in BigEndian format. This is what the code segment
    // below describes.
    // RFC: (https://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-01#section-2.1)
    std::array<uint8_t, sizeof(uint64_t)> dataLenBitsEncodedStorage;
    DataRange dataLenBitsEncoded(dataLenBitsEncodedStorage);
    dataLenBitsEncoded.write<BigEndian<uint64_t>>(static_cast<uint64_t>(0));

    ConstDataRange keyCDR(key.getKey(), kAeadAesHmacKeySize);
    ConstDataRange empty(nullptr, 0);

    return aeadEncryptWithIV(keyCDR, in, empty, empty, dataLenBitsEncoded, out);
}

Status aeadEncryptDataFrame(FLEEncryptionFrame& dataframe) {
    auto associatedData = dataframe.getAssociatedData();
    if (associatedData.length() >= kMaxAssociatedDataLength) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "AssociatedData for encryption is too large. Cannot be larger than "
                          << kMaxAssociatedDataLength << " bytes.");
    }

    // According to the rfc on AES encryption, the associatedDataLength is defined as the
    // number of bits in associatedData in BigEndian format. This is what the code segment
    // below describes.
    // RFC: (https://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-01#section-2.1)
    std::array<uint8_t, sizeof(uint64_t)> dataLenBitsEncodedStorage;
    DataRange dataLenBitsEncoded(dataLenBitsEncodedStorage);
    dataLenBitsEncoded.write<BigEndian<uint64_t>>(static_cast<uint64_t>(associatedData.length()) *
                                                  8);


    auto key = dataframe.getKey();

    auto plaintext = dataframe.getPlaintext();

    if (key->getKeySize() != kFieldLevelEncryptionKeySize) {
        return Status(ErrorCodes::BadValue, "Invalid key size.");
    }

    if (plaintext.data() == nullptr) {
        return Status(ErrorCodes::BadValue, "Invalid AEAD plaintext input.");
    }

    if (key->getAlgorithm() != aesAlgorithm) {
        return Status(ErrorCodes::BadValue, "Invalid algorithm for key.");
    }

    ConstDataRange iv(nullptr, 0);
    SHA512Block hmacOutput;

    if (dataframe.getFLEAlgorithmType() == FleAlgorithmInt::kDeterministic) {
        const uint8_t* ivKey = key->getKey() + kAeadAesHmacKeySize;
        hmacOutput = SHA512Block::computeHmac(
            ivKey, sym256KeySize, {associatedData, dataLenBitsEncoded, plaintext});

        static_assert(SHA512Block::kHashLength >= kIVSize,
                      "Invalid AEAD parameters. Generated IV too short.");

        iv = ConstDataRange(hmacOutput.data(), kIVSize);
    }

    ConstDataRange aeadKey(key->getKey(), kAeadAesHmacKeySize);
    DataRange out(dataframe.getCiphertextMutable(), dataframe.getDataLength());
    return aeadEncryptWithIV(aeadKey, plaintext, iv, associatedData, dataLenBitsEncoded, out);
}

Status aeadEncryptWithIV(ConstDataRange key,
                         ConstDataRange in,
                         ConstDataRange iv,
                         ConstDataRange associatedData,
                         ConstDataRange dataLenBitsEncoded,
                         DataRange out) {
    if (key.length() != kAeadAesHmacKeySize) {
        return Status(ErrorCodes::BadValue, "Invalid key size.");
    }

    if (!(in.length() && out.length())) {
        return Status(ErrorCodes::BadValue, "Invalid AEAD parameters.");
    }

    if (out.length() != aeadCipherOutputLength(in.length())) {
        return Status(ErrorCodes::BadValue, "Invalid output buffer size.");
    }

    if (associatedData.length() >= kMaxAssociatedDataLength) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "AssociatedData for encryption is too large. Cannot be larger than "
                          << kMaxAssociatedDataLength << " bytes.");
    }

    bool ivProvided = false;
    if (iv.length() != 0) {
        invariant(iv.length() == 16);
        out.write(iv);
        ivProvided = true;
    }

    const auto* macKey = reinterpret_cast<const std::uint8_t*>(key.data());
    const auto* encKey = reinterpret_cast<const std::uint8_t*>(key.data() + sym256KeySize);

    SymmetricKey symEncKey(encKey, sym256KeySize, aesAlgorithm, "aesKey", 1);
    std::size_t aesOutLen = out.length() - kHmacOutSize;

    auto swEncrypt = _aesEncrypt(symEncKey, in, {out.data(), aesOutLen}, ivProvided);
    if (!swEncrypt.isOK()) {
        return swEncrypt.getStatus();
    }

    // Split `out` into two separate ranges.
    // One for the just written ciphertext,
    // and another for the HMAC signature on the end.
    DataRangeCursor outCursor(out);
    auto cipherTextRange = outCursor.sliceAndAdvance(swEncrypt.getValue());

    SHA512Block hmacOutput = SHA512Block::computeHmac(
        macKey, sym256KeySize, {associatedData, cipherTextRange, dataLenBitsEncoded});

    // We intentionally only write the first 256 bits of the digest produced by SHA512.
    ConstDataRange truncatedHash(hmacOutput.data(), kHmacOutSize);
    outCursor.writeAndAdvance(truncatedHash);

    return Status::OK();
}

StatusWith<std::size_t> aeadDecrypt(const SymmetricKey& key,
                                    ConstDataRange in,
                                    ConstDataRange associatedData,
                                    DataRange out) {
    if (key.getKeySize() < kAeadAesHmacKeySize) {
        return Status(ErrorCodes::BadValue, "Invalid key size.");
    }

    if (!out.length()) {
        return Status(ErrorCodes::BadValue, "Invalid AEAD parameters.");
    }

    if (in.length() < kHmacOutSize) {
        return Status(ErrorCodes::BadValue, "Ciphertext is not long enough.");
    }

    size_t expectedMaximumPlainTextSize =
        uassertStatusOK(aeadGetMaximumPlainTextLength(in.length()));
    if (out.length() != expectedMaximumPlainTextSize) {
        return Status(ErrorCodes::BadValue, "Output buffer must be as long as the cipherText.");
    }

    if (associatedData.length() >= kMaxAssociatedDataLength) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "AssociatedData for encryption is too large. Cannot be larger than "
                          << kMaxAssociatedDataLength << " bytes.");
    }

    const uint8_t* macKey = key.getKey();
    const uint8_t* encKey = key.getKey() + sym256KeySize;

    // Split input into actual ciphertext, and the HMAC bit at the end.
    auto [cipherText, hmacRange] = in.split(in.length() - kHmacOutSize);

    // According to the rfc on AES encryption, the associatedDataLength is defined as the
    // number of bits in associatedData in BigEndian format. This is what the code segment
    // below describes.
    std::array<uint8_t, sizeof(uint64_t)> dataLenBitsEncodedStorage;
    DataRange dataLenBitsEncoded(dataLenBitsEncodedStorage);
    dataLenBitsEncoded.write<BigEndian<uint64_t>>(associatedData.length() * 8);

    SHA512Block hmacOutput = SHA512Block::computeHmac(
        macKey, sym256KeySize, {associatedData, cipherText, dataLenBitsEncoded});

    // Note that while we produce a 512bit digest with SHA512,
    // we only store and validate the first 256 bits (32 bytes).
    if (consttimeMemEqual(reinterpret_cast<const unsigned char*>(hmacOutput.data()),
                          hmacRange.data<unsigned char>(),
                          kHmacOutSize) == false) {
        return Status(ErrorCodes::BadValue, "HMAC data authentication failed.");
    }

    SymmetricKey symEncKey(encKey, sym256KeySize, aesAlgorithm, key.getKeyId(), 1);

    return _aesDecrypt(symEncKey, cipherText, out);
}

Status aeadDecryptDataFrame(FLEDecryptionFrame& dataframe) {
    auto ciphertext = dataframe.getCiphertext();
    auto associatedData = dataframe.getAssociatedData();
    auto& plaintext = dataframe.getPlaintextMutable();
    auto swPlainSize = aeadDecrypt(*dataframe.getKey(), ciphertext, associatedData, plaintext);
    if (swPlainSize.isOK()) {
        plaintext.resize(swPlainSize.getValue());
    } else {
        plaintext.resize(0);
    }

    return swPlainSize.getStatus();
}

StatusWith<std::size_t> aeadDecryptLocalKMS(const SymmetricKey& key,
                                            ConstDataRange cipher,
                                            DataRange out) {
    return aeadDecrypt(key, cipher, {nullptr, 0}, out);
}

}  // namespace crypto
}  // namespace mongo
