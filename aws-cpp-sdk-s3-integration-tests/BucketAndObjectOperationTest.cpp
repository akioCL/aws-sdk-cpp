/*
* Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License").
* You may not use this file except in compliance with the License.
* A copy of the License is located at
*
*  http://aws.amazon.com/apache2.0
*
* or in the "license" file accompanying this file. This file is distributed
* on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
* express or implied. See the License for the specific language governing
* permissions and limitations under the License.
*/

#include <aws/external/gtest.h>
#include <aws/testing/ProxyConfig.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/utils/Outcome.h>
#include <aws/s3/S3Client.h>
#include <aws/core/utils/ratelimiter/DefaultRateLimiter.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/core/utils/DateTime.h>

#include <fstream>

#ifdef _WIN32
#pragma warning(disable: 4127)
#endif //_WIN32

using namespace Aws::Auth;
using namespace Aws::Http;
using namespace Aws::Client;
using namespace Aws::S3;
using namespace Aws::S3::Model;
using namespace Aws::Utils;

namespace
{

static const char* ALLOCATION_TAG = "BucketAndObjectOperationTest";
static const char* CREATE_BUCKET_TEST_NAME = "awsnativesdkcreatebuckettestbucket";
static const char* PUT_OBJECTS_BUCKET_NAME = "awsnativesdkputobjectstestbucket";
static const char* PUT_MULTIPART_BUCKET_NAME = "awsnativesdkputobjectmultipartbucket";
static const char* ERRORS_TESTING_BUCKET = "awsnativesdkerrorsbucket";
static const char* TEST_OBJ_KEY = "TestObjectKey";

static const int TIMEOUT_MAX = 10;

class BucketAndObjectOperationTest : public ::testing::Test
{
public:
    static std::shared_ptr<S3Client> Client;
    static std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface> Limiter;
    static Aws::String TimeStamp;

protected:

    static void SetUpTestCase()
    {
        Limiter = Aws::MakeShared<Aws::Utils::RateLimits::DefaultRateLimiter<>>(ALLOCATION_TAG, 50000000);

        // Create a client
        ClientConfiguration config;
        config.scheme = Scheme::HTTP;
        config.connectTimeoutMs = 30000;
        config.requestTimeoutMs = 30000;
        config.readRateLimiter = Limiter;
        config.writeRateLimiter = Limiter;

        //to use a proxy, uncomment the next two lines.
        if(USE_PROXY_FOR_TESTS)
        {
            config.proxyHost = PROXY_HOST;
            config.proxyPort = PROXY_PORT;
        }

        Client = Aws::MakeShared<S3Client>(ALLOCATION_TAG, config);
        DeleteBucket(CalculateBucketName(CREATE_BUCKET_TEST_NAME));
        DeleteBucket(CalculateBucketName(PUT_OBJECTS_BUCKET_NAME));
        DeleteBucket(CalculateBucketName(PUT_MULTIPART_BUCKET_NAME));
        DeleteBucket(CalculateBucketName(ERRORS_TESTING_BUCKET));
    }

    static void TearDownTestCase()
    {
        DeleteBucket(CalculateBucketName(CREATE_BUCKET_TEST_NAME));
        DeleteBucket(CalculateBucketName(PUT_OBJECTS_BUCKET_NAME));
        DeleteBucket(CalculateBucketName(PUT_MULTIPART_BUCKET_NAME));
        DeleteBucket(CalculateBucketName(ERRORS_TESTING_BUCKET));
        Limiter = nullptr;
        Client = nullptr;
    }

    static std::shared_ptr<Aws::StringStream> Create5MbStreamForUploadPart(const char* partTag)
    {
        std::shared_ptr<Aws::StringStream> streamPtr = Aws::MakeShared<Aws::StringStream>(ALLOCATION_TAG);
        unsigned fiveMbSize = 5 * 1024 * 1024;
        for (unsigned i = 0; i < fiveMbSize; i += 30)
        {
            *streamPtr << "Multi-Part upload Test Part " << partTag << ":" << std::endl;
        }

        streamPtr->seekg(0);

        return streamPtr;
    }

    static UploadPartOutcomeCallable MakeUploadPartOutcomeAndGetCallable(unsigned partNumber, const ByteBuffer& md5OfStream, const std::shared_ptr<Aws::IOStream>& partStream,
                                                                         const Aws::String& bucketName, const char* objectName, const Aws::String& uploadId)
    {
        UploadPartRequest uploadPart1Request;
        uploadPart1Request.SetBucket(bucketName);
        uploadPart1Request.SetKey(objectName);
        uploadPart1Request.SetPartNumber(partNumber);
        uploadPart1Request.SetUploadId(uploadId);
        uploadPart1Request.SetBody(partStream);
        uploadPart1Request.SetContentMD5(HashingUtils::Base64Encode(md5OfStream));

        auto startingPoint = partStream->tellg();
        partStream->seekg(0LL, partStream->end);
        uploadPart1Request.SetContentLength(static_cast<long>(partStream->tellg()));
        partStream->seekg(startingPoint);

        return Client->UploadPartCallable(uploadPart1Request);
    }

    static void VerifyUploadPartOutcome(UploadPartOutcome& outcome, const ByteBuffer& md5OfStream)
    {
        ASSERT_TRUE(outcome.IsSuccess());
        Aws::StringStream ss;
        ss << "\"" << HashingUtils::HexEncode(md5OfStream) << "\"";
        ASSERT_EQ(ss.str(), outcome.GetResult().GetETag());
    }

    static bool WaitForBucketToPropagate(const Aws::String& bucketName)
    {
        unsigned timeoutCount = 0;
        while (timeoutCount++ < TIMEOUT_MAX)
        {
            HeadBucketRequest headBucketRequest;
            headBucketRequest.SetBucket(bucketName);
            HeadBucketOutcome headBucketOutcome = Client->HeadBucket(headBucketRequest);
            if (headBucketOutcome.IsSuccess())
            {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        return false;
    }

    static bool WaitForObjectToPropagate(const Aws::String& bucketName, const char* objectKey)
    {
        unsigned timeoutCount = 0;
        while (timeoutCount++ < TIMEOUT_MAX)
        {
            HeadObjectRequest headObjectRequest;
            headObjectRequest.SetBucket(bucketName);
            headObjectRequest.SetKey(objectKey);
            HeadObjectOutcome headObjectOutcome = Client->HeadObject(headObjectRequest);
            if (headObjectOutcome.IsSuccess())
            {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        return false;
    }

    static void EmptyBucket(const Aws::String& bucketName)
    {
        ListObjectsRequest listObjectsRequest;
        listObjectsRequest.SetBucket(bucketName);

        ListObjectsOutcome listObjectsOutcome = Client->ListObjects(listObjectsRequest);

        if (!listObjectsOutcome.IsSuccess())
            return;

        for (const auto& object : listObjectsOutcome.GetResult().GetContents())
        {
            DeleteObjectRequest deleteObjectRequest;
            deleteObjectRequest.SetBucket(bucketName);
            deleteObjectRequest.SetKey(object.GetKey());
            Client->DeleteObject(deleteObjectRequest);
        }
    }

    static void WaitForBucketToEmpty(const Aws::String& bucketName)
    {
        ListObjectsRequest listObjectsRequest;
        listObjectsRequest.SetBucket(bucketName);

        unsigned checkForObjectsCount = 0;
        while (checkForObjectsCount++ < TIMEOUT_MAX)
        {
            ListObjectsOutcome listObjectsOutcome = Client->ListObjects(listObjectsRequest);
            ASSERT_TRUE(listObjectsOutcome.IsSuccess());

            if (listObjectsOutcome.GetResult().GetContents().size() > 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            else
            {
                break;
            }
        }
    }

    static void DeleteBucket(const Aws::String& bucketName)
    {
        HeadBucketRequest headBucketRequest;
        headBucketRequest.SetBucket(bucketName);
        HeadBucketOutcome bucketOutcome = Client->HeadBucket(headBucketRequest);

        if (bucketOutcome.IsSuccess())
        {
            EmptyBucket(bucketName);
            WaitForBucketToEmpty(bucketName);

            DeleteBucketRequest deleteBucketRequest;
            deleteBucketRequest.SetBucket(bucketName);

            DeleteBucketOutcome deleteBucketOutcome = Client->DeleteBucket(deleteBucketRequest);
            ASSERT_TRUE(deleteBucketOutcome.IsSuccess());
        }
    }

    static Aws::String CalculateBucketName(const char* bucketPrefix)
    {
      return bucketPrefix + TimeStamp;
    }
};

std::shared_ptr<S3Client> BucketAndObjectOperationTest::Client(nullptr);
std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface> BucketAndObjectOperationTest::Limiter(nullptr);
Aws::String BucketAndObjectOperationTest::TimeStamp(DateTime::CalculateGmtTimestampAsString("%Y%m%dT%H%M%SZ"));

TEST_F(BucketAndObjectOperationTest, TestBucketCreationAndListing)
{
    Aws::String fullBucketName = CalculateBucketName(CREATE_BUCKET_TEST_NAME);
    HeadBucketRequest headBucketRequest;
    headBucketRequest.SetBucket(fullBucketName);
    HeadBucketOutcome headBucketOutcome = Client->HeadBucket(headBucketRequest);
    ASSERT_FALSE(headBucketOutcome.IsSuccess());

    CreateBucketRequest createBucketRequest;
    createBucketRequest.SetBucket(fullBucketName);
    createBucketRequest.SetACL(BucketCannedACL::public_read_write);

    CreateBucketOutcome createBucketOutcome = Client->CreateBucket(createBucketRequest);
    ASSERT_TRUE(createBucketOutcome.IsSuccess());
    const CreateBucketResult& createBucketResult = createBucketOutcome.GetResult();
    ASSERT_FALSE(createBucketResult.GetLocation().empty());
    ASSERT_TRUE(WaitForBucketToPropagate(fullBucketName));

    ListBucketsOutcome listBucketsOutcome = Client->ListBuckets();
    ASSERT_TRUE(listBucketsOutcome.IsSuccess());
    ASSERT_TRUE(listBucketsOutcome.GetResult().GetBuckets().size() >= 1);

    bool foundBucket(false);

    for (const auto& bucket : listBucketsOutcome.GetResult().GetBuckets())
    {
        if (bucket.GetName() == fullBucketName)
        {
            foundBucket = true;
        }
    }

    ASSERT_TRUE(foundBucket);

    DeleteBucketRequest deleteBucketRequest;
    deleteBucketRequest.SetBucket(fullBucketName);
    DeleteBucketOutcome deleteBucketOutcome = Client->DeleteBucket(deleteBucketRequest);
    ASSERT_TRUE(deleteBucketOutcome.IsSuccess());

    headBucketRequest.SetBucket(fullBucketName);
    headBucketOutcome = Client->HeadBucket(headBucketRequest);

    unsigned timeoutCount = 0;
    bool bucketHeadSucceeded(true);
    while (timeoutCount++ < TIMEOUT_MAX)
    {
        headBucketOutcome = Client->HeadBucket(headBucketRequest);
        if (!headBucketOutcome.IsSuccess())
        {
            bucketHeadSucceeded = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    ASSERT_FALSE(bucketHeadSucceeded);
}

TEST_F(BucketAndObjectOperationTest, TestObjectOperations)
{
    Aws::String fullBucketName = CalculateBucketName(PUT_OBJECTS_BUCKET_NAME);

    CreateBucketRequest createBucketRequest;
    createBucketRequest.SetBucket(fullBucketName);
    createBucketRequest.SetACL(BucketCannedACL::public_read_write);

    CreateBucketOutcome createBucketOutcome = Client->CreateBucket(createBucketRequest);
    ASSERT_TRUE(createBucketOutcome.IsSuccess());
    const CreateBucketResult& createBucketResult = createBucketOutcome.GetResult();
    ASSERT_TRUE(!createBucketResult.GetLocation().empty());

    WaitForBucketToPropagate(fullBucketName);

    PutObjectRequest putObjectRequest;
    putObjectRequest.SetBucket(fullBucketName);

    std::shared_ptr<Aws::IOStream> objectStream = Aws::MakeShared<Aws::StringStream>("BucketAndObjectOperationTest");
    *objectStream << "Test Object";
    objectStream->flush();
    putObjectRequest.SetBody(objectStream);
    putObjectRequest.SetContentLength(static_cast<long>(putObjectRequest.GetBody()->tellp()));
    putObjectRequest.SetContentMD5(HashingUtils::Base64Encode(HashingUtils::CalculateMD5(*putObjectRequest.GetBody())));
    putObjectRequest.SetContentType("text/plain");
    putObjectRequest.SetKey(TEST_OBJ_KEY);

    PutObjectOutcome putObjectOutcome = Client->PutObject(putObjectRequest);
    ASSERT_TRUE(putObjectOutcome.IsSuccess());

    //verify md5 sums between what was sent and what s3 told us they received.
    Aws::StringStream ss;
    ss << "\"" << HashingUtils::HexEncode(HashingUtils::CalculateMD5(*putObjectRequest.GetBody())) << "\"";
    ASSERT_EQ(ss.str(), putObjectOutcome.GetResult().GetETag());

    WaitForObjectToPropagate(fullBucketName, TEST_OBJ_KEY);

    ListObjectsRequest listObjectsRequest;
    listObjectsRequest.SetBucket(fullBucketName);

    ListObjectsOutcome listObjectsOutcome = Client->ListObjects(listObjectsRequest);
    ASSERT_TRUE(listObjectsOutcome.IsSuccess());
    ASSERT_TRUE(WaitForObjectToPropagate(fullBucketName, TEST_OBJ_KEY));

    GetObjectRequest getObjectRequest;
    getObjectRequest.SetBucket(fullBucketName);
    getObjectRequest.SetKey(TEST_OBJ_KEY);

    GetObjectOutcome getObjectOutcome = Client->GetObject(getObjectRequest);
    ASSERT_TRUE(getObjectOutcome.IsSuccess());
    ss.str("");
    ss << getObjectOutcome.GetResult().GetBody().rdbuf();
    ASSERT_EQ("Test Object", ss.str());

    HeadObjectRequest headObjectRequest;
    headObjectRequest.SetBucket(fullBucketName);
    headObjectRequest.SetKey(TEST_OBJ_KEY);

    HeadObjectOutcome headObjectOutcome = Client->HeadObject(headObjectRequest);
    ASSERT_TRUE(headObjectOutcome.IsSuccess());

    //verify md5 sums between what was sent and what the file s3 gave us back.
    ss.str("");
    ss << "\"" << HashingUtils::HexEncode(HashingUtils::CalculateMD5(*putObjectRequest.GetBody())) << "\"";
    ASSERT_EQ(ss.str(), getObjectOutcome.GetResult().GetETag());

    DeleteObjectRequest deleteObjectRequest;
    deleteObjectRequest.SetBucket(fullBucketName);
    deleteObjectRequest.SetKey(TEST_OBJ_KEY);
    DeleteObjectOutcome deleteObjectOutcome = Client->DeleteObject(deleteObjectRequest);
    ASSERT_TRUE(deleteObjectOutcome.IsSuccess());

    WaitForBucketToEmpty(fullBucketName);

    headObjectOutcome = Client->HeadObject(headObjectRequest);
    ASSERT_FALSE(headObjectOutcome.IsSuccess());
}

TEST_F(BucketAndObjectOperationTest, TestMultiPartObjectOperations)
{
    const char* multipartKeyName = "MultiPartKey";
    Aws::String fullBucketName = CalculateBucketName(PUT_MULTIPART_BUCKET_NAME);
    CreateBucketRequest createBucketRequest;
    createBucketRequest.SetBucket(fullBucketName);
    createBucketRequest.SetACL(BucketCannedACL::public_read_write);

    CreateBucketOutcome createBucketOutcome = Client->CreateBucket(createBucketRequest);
    ASSERT_TRUE(createBucketOutcome.IsSuccess());
    const CreateBucketResult& createBucketResult = createBucketOutcome.GetResult();
    ASSERT_TRUE(!createBucketResult.GetLocation().empty());

    WaitForBucketToPropagate(fullBucketName);

    CreateMultipartUploadRequest createMultipartUploadRequest;
    createMultipartUploadRequest.SetBucket(fullBucketName);
    createMultipartUploadRequest.SetKey(multipartKeyName);
    createMultipartUploadRequest.SetContentType("text/plain");

    CreateMultipartUploadOutcome createMultipartUploadOutcome = Client->CreateMultipartUpload(
            createMultipartUploadRequest);
    ASSERT_TRUE(createMultipartUploadOutcome.IsSuccess());

    std::shared_ptr<Aws::IOStream> part1Stream = Create5MbStreamForUploadPart("1");
    ByteBuffer part1Md5(HashingUtils::CalculateMD5(*part1Stream));
    UploadPartOutcomeCallable uploadPartOutcomeCallable1 =
            MakeUploadPartOutcomeAndGetCallable(1, part1Md5, part1Stream, fullBucketName,
                                                multipartKeyName, createMultipartUploadOutcome.GetResult().GetUploadId());

    std::shared_ptr<Aws::IOStream> part2Stream = Create5MbStreamForUploadPart("2");
    ByteBuffer part2Md5(HashingUtils::CalculateMD5(*part2Stream));
    UploadPartOutcomeCallable uploadPartOutcomeCallable2 =
            MakeUploadPartOutcomeAndGetCallable(2, part2Md5, part2Stream, fullBucketName,
                                                multipartKeyName,
                                                createMultipartUploadOutcome.GetResult().GetUploadId());

    std::shared_ptr<Aws::IOStream> part3Stream = Create5MbStreamForUploadPart("3");
    ByteBuffer part3Md5(HashingUtils::CalculateMD5(*part3Stream));
    UploadPartOutcomeCallable uploadPartOutcomeCallable3 =
            MakeUploadPartOutcomeAndGetCallable(3, part3Md5, part3Stream, fullBucketName,
                                                multipartKeyName,
                                                createMultipartUploadOutcome.GetResult().GetUploadId());

    UploadPartOutcome uploadPartOutcome1 = uploadPartOutcomeCallable1.get();
    UploadPartOutcome uploadPartOutcome2 = uploadPartOutcomeCallable2.get();
    UploadPartOutcome uploadPartOutcome3 = uploadPartOutcomeCallable3.get();

    VerifyUploadPartOutcome(uploadPartOutcome1, part1Md5);

    CompletedPart completedPart1;
    completedPart1.SetETag(uploadPartOutcome1.GetResult().GetETag());
    completedPart1.SetPartNumber(1);

    VerifyUploadPartOutcome(uploadPartOutcome2, part2Md5);

    CompletedPart completedPart2;
    completedPart2.SetETag(uploadPartOutcome2.GetResult().GetETag());
    completedPart2.SetPartNumber(2);

    VerifyUploadPartOutcome(uploadPartOutcome3, part3Md5);

    CompletedPart completedPart3;
    completedPart3.SetETag(uploadPartOutcome3.GetResult().GetETag());
    completedPart3.SetPartNumber(3);

    CompleteMultipartUploadRequest completeMultipartUploadRequest;
    completeMultipartUploadRequest.SetBucket(fullBucketName);
    completeMultipartUploadRequest.SetKey(multipartKeyName);
    completeMultipartUploadRequest.SetUploadId(createMultipartUploadOutcome.GetResult().GetUploadId());

    CompletedMultipartUpload completedMultipartUpload;
    completedMultipartUpload.AddParts(completedPart1);
    completedMultipartUpload.AddParts(completedPart2);
    completedMultipartUpload.AddParts(completedPart3);
    completeMultipartUploadRequest.WithMultipartUpload(completedMultipartUpload);

    CompleteMultipartUploadOutcome completeMultipartUploadOutcome = Client->CompleteMultipartUpload(
            completeMultipartUploadRequest);
    ASSERT_TRUE(completeMultipartUploadOutcome.IsSuccess());

    WaitForObjectToPropagate(fullBucketName, multipartKeyName);

    GetObjectRequest getObjectRequest;
    getObjectRequest.SetBucket(fullBucketName);
    getObjectRequest.SetKey(multipartKeyName);

    GetObjectOutcome getObjectOutcome = Client->GetObject(getObjectRequest);
    ASSERT_TRUE(getObjectOutcome.IsSuccess());

    Aws::StringStream expectedStreamValue;
    part1Stream->seekg(0, part1Stream->beg);
    part2Stream->seekg(0, part2Stream->beg);
    part3Stream->seekg(0, part3Stream->beg);
    expectedStreamValue << part1Stream->rdbuf() << part2Stream->rdbuf() << part3Stream->rdbuf();

    Aws::StringStream actualStreamValue;
    actualStreamValue << getObjectOutcome.GetResult().GetBody().rdbuf();
    ASSERT_EQ(expectedStreamValue.str(), actualStreamValue.str());

    // repeat the get, but channel it directly to a file; tests the ability to override the output stream
#ifndef __ANDROID__
    static const char* DOWNLOADED_FILENAME = "DownloadTestFile";

    std::remove(DOWNLOADED_FILENAME);

    GetObjectRequest getObjectRequest2;
    getObjectRequest2.SetBucket(fullBucketName);
    getObjectRequest2.SetKey(multipartKeyName);
    getObjectRequest2.SetResponseStreamFactory([](){ return Aws::New<Aws::FStream>( ALLOCATION_TAG, DOWNLOADED_FILENAME, std::ios_base::out ); });

    {
        // Enclose scope just to make sure the download file is properly closed before we reread it
        GetObjectOutcome getObjectOutcome2 = Client->GetObject(getObjectRequest2);
        ASSERT_TRUE(getObjectOutcome2.IsSuccess());
    }

    Aws::String fileContents;
    Aws::IFStream downloadedFile(DOWNLOADED_FILENAME);
    ASSERT_TRUE(downloadedFile.good());

    if(downloadedFile.good())
    {
        downloadedFile.seekg(0, std::ios::end);   
        fileContents.reserve(static_cast<uint32_t>(downloadedFile.tellg()));
        downloadedFile.seekg(0, std::ios::beg);
        fileContents.assign((std::istreambuf_iterator<char>(downloadedFile)), std::istreambuf_iterator<char>());
    }

    std::remove(DOWNLOADED_FILENAME);

    ASSERT_EQ(expectedStreamValue.str(), fileContents);

#endif // __ANDROID__

    // Remove the file
    DeleteObjectRequest deleteObjectRequest;
    deleteObjectRequest.SetBucket(fullBucketName);
    deleteObjectRequest.SetKey(multipartKeyName);

    DeleteObjectOutcome deleteObjectOutcome = Client->DeleteObject(deleteObjectRequest);
    ASSERT_TRUE(deleteObjectOutcome.IsSuccess());
}

TEST_F(BucketAndObjectOperationTest, TestThatErrorsParse)
{
    Aws::String fullBucketName = CalculateBucketName(ERRORS_TESTING_BUCKET);

    ListObjectsRequest listObjectsRequest;
    listObjectsRequest.SetBucket("Non-Existent");

    ListObjectsOutcome listObjectsOutcome = Client->ListObjects(listObjectsRequest);
    ASSERT_FALSE(listObjectsOutcome.IsSuccess());
    ASSERT_EQ(S3Errors::NO_SUCH_BUCKET, listObjectsOutcome.GetError().GetErrorType());

    CreateBucketRequest createBucketRequest;
    createBucketRequest.SetBucket(fullBucketName);

    CreateBucketOutcome createBucketOutcome = Client->CreateBucket(createBucketRequest);
    ASSERT_TRUE(createBucketOutcome.IsSuccess());
    WaitForBucketToPropagate(fullBucketName);

    GetObjectRequest getObjectRequest;
    getObjectRequest.SetBucket(fullBucketName);
    getObjectRequest.SetKey("non-Existent");
    GetObjectOutcome getObjectOutcome = Client->GetObject(getObjectRequest);
    ASSERT_FALSE(getObjectOutcome.IsSuccess());
    ASSERT_EQ(S3Errors::NO_SUCH_KEY, getObjectOutcome.GetError().GetErrorType());
}

}

