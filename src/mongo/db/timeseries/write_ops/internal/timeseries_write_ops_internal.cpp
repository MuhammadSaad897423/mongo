/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/write_ops/write_ops_exec_util.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::write_ops {

namespace {

MONGO_FAIL_POINT_DEFINE(failAtomicTimeseriesWrites);
MONGO_FAIL_POINT_DEFINE(failUnorderedTimeseriesInsert);
MONGO_FAIL_POINT_DEFINE(hangInsertIntoBucketCatalogBeforeCheckingTimeseriesCollection);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeCommit);


using TimeseriesBatches =
    std::vector<std::pair<std::shared_ptr<bucket_catalog::WriteBatch>, size_t>>;
using TimeseriesStmtIds = timeseries::TimeseriesStmtIds;

struct TimeseriesSingleWriteResult {
    StatusWith<SingleWriteResult> result;
    bool canContinue = true;
};

bool isTimeseriesWriteRetryable(OperationContext* opCtx) {
    return (opCtx->getTxnNumber() && !opCtx->inMultiDocumentTransaction());
}

std::shared_ptr<bucket_catalog::WriteBatch>& extractFromPair(
    std::pair<std::shared_ptr<bucket_catalog::WriteBatch>, size_t>& pair) {
    return pair.first;
}

TimeseriesSingleWriteResult getTimeseriesSingleWriteResult(
    write_ops_exec::WriteResult&& reply, const mongo::write_ops::InsertCommandRequest& request) {
    invariant(reply.results.size() == 1,
              str::stream() << "Unexpected number of results (" << reply.results.size()
                            << ") for insert on time-series collection "
                            << internal::ns(request).toStringForErrorMsg());

    return {std::move(reply.results[0]), reply.canContinue};
}

boost::optional<std::pair<Status, bool>> checkFailUnorderedTimeseriesInsertFailPoint(
    const BSONObj& metadata) {
    bool canContinue = true;
    if (MONGO_unlikely(failUnorderedTimeseriesInsert.shouldFail(
            [&metadata, &canContinue](const BSONObj& data) {
                BSONElementComparator comp(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
                if (auto continueElem = data["canContinue"]) {
                    canContinue = data["canContinue"].trueValue();
                }
                return comp.compare(data["metadata"], metadata.firstElement()) == 0;
            }))) {
        return std::make_pair(Status(ErrorCodes::FailPointEnabled,
                                     "Failed unordered time-series insert due to "
                                     "failUnorderedTimeseriesInsert fail point"),
                              canContinue);
    }
    return boost::none;
}

/**
 * Returns the status and whether the request can continue.
 */
TimeseriesSingleWriteResult performTimeseriesInsert(
    OperationContext* opCtx,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds,
    const mongo::write_ops::InsertCommandRequest& request,
    std::shared_ptr<bucket_catalog::WriteBatch> batch) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
        return {status->first, status->second};
    }
    return getTimeseriesSingleWriteResult(
        write_ops_exec::performInserts(
            opCtx,
            write_ops_utils::makeTimeseriesInsertOp(
                batch,
                write_ops_utils::makeTimeseriesBucketsNamespace(internal::ns(request)),
                metadata,
                std::move(stmtIds)),
            OperationSource::kTimeseriesInsert),
        request);
}

/**
 * Returns the status and whether the request can continue.
 */
TimeseriesSingleWriteResult performTimeseriesUpdate(
    OperationContext* opCtx,
    const BSONObj& metadata,
    const mongo::write_ops::UpdateCommandRequest& op,
    const mongo::write_ops::InsertCommandRequest& request) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
        return {status->first, status->second};
    }
    return getTimeseriesSingleWriteResult(
        write_ops_exec::performUpdates(opCtx, op, OperationSource::kTimeseriesInsert), request);
}

/**
 * Throws if compression fails for any reason.
 */
void compressUncompressedBucketOnReopen(OperationContext* opCtx,
                                        const bucket_catalog::BucketId& bucketId,
                                        const NamespaceString& nss,
                                        StringData timeFieldName) {
    bool validateCompression = gValidateTimeseriesCompression.load();

    auto bucketCompressionFunc = [&](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
        auto compressed =
            timeseries::compressBucket(bucketDoc, timeFieldName, nss, validateCompression);

        if (!compressed.compressedBucket) {
            uassert(timeseries::BucketCompressionFailure(
                        bucketId.collectionUUID, bucketId.oid, bucketId.keySignature),
                    "Time-series compression failed on reopened bucket",
                    compressed.compressedBucket);
        }

        return compressed.compressedBucket;
    };

    mongo::write_ops::UpdateModification u(std::move(bucketCompressionFunc));
    mongo::write_ops::UpdateOpEntry update(BSON("_id" << bucketId.oid), std::move(u));
    invariant(!update.getMulti(), bucketId.oid.toString());
    invariant(!update.getUpsert(), bucketId.oid.toString());

    mongo::write_ops::UpdateCommandRequest compressionOp(nss, {update});
    mongo::write_ops::WriteCommandRequestBase base;
    // The schema validation configured in the bucket collection is intended for direct
    // operations by end users and is not applicable here.
    base.setBypassDocumentValidation(true);
    // Timeseries compression operation is not a user operation and should not use a
    // statement id from any user op. Set to Uninitialized to bypass.
    base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});
    compressionOp.setWriteCommandRequestBase(std::move(base));
    auto result =
        write_ops_exec::performUpdates(opCtx, compressionOp, OperationSource::kTimeseriesInsert);
    invariant(result.results.size() == 1,
              str::stream() << "Unexpected number of results (" << result.results.size()
                            << ") for update on time-series collection "
                            << nss.toStringForErrorMsg());
}

// For sharded time-series collections, we need to use the granularity from the config
// server (through shard filtering information) as the source of truth for the current
// granularity value, due to the possible inconsistency in the process of granularity
// updates.
void rebuildOptionsWithGranularityFromConfigServer(OperationContext* opCtx,
                                                   const NamespaceString& bucketsNs,
                                                   TimeseriesOptions& timeSeriesOptions) {
    auto collDesc = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, bucketsNs)
                        ->getCollectionDescription(opCtx);
    if (collDesc.isSharded()) {
        tassert(6102801,
                "Sharded time-series buckets collection is missing time-series fields",
                collDesc.getTimeseriesFields());
        auto granularity = collDesc.getTimeseriesFields()->getGranularity();
        auto bucketMaxSpanSeconds = collDesc.getTimeseriesFields()->getBucketMaxSpanSeconds();

        if (granularity) {
            timeSeriesOptions.setGranularity(granularity.get());
            timeSeriesOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(*granularity));
            timeSeriesOptions.setBucketRoundingSeconds(
                timeseries::getBucketRoundingSecondsFromGranularity(*granularity));
        } else if (!bucketMaxSpanSeconds) {
            timeSeriesOptions.setGranularity(BucketGranularityEnum::Seconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(*timeSeriesOptions.getGranularity()));
            timeSeriesOptions.setBucketRoundingSeconds(
                timeseries::getBucketRoundingSecondsFromGranularity(
                    *timeSeriesOptions.getGranularity()));
        } else {
            invariant(bucketMaxSpanSeconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(bucketMaxSpanSeconds);

            auto bucketRoundingSeconds = collDesc.getTimeseriesFields()->getBucketRoundingSeconds();
            invariant(bucketRoundingSeconds);
            timeSeriesOptions.setBucketRoundingSeconds(bucketRoundingSeconds);
        }
    }
}

/**
 * Stages writes to the system.buckets collection, which may have the side effect of reopening an
 * existing bucket to put the measurement(s) into as well as closing buckets. Returns info about the
 * write which is needed for committing the write.
 */
std::tuple<boost::optional<UUID>, TimeseriesBatches, TimeseriesStmtIds, size_t /* numInserted */>
insertIntoBucketCatalog(OperationContext* opCtx,
                        const mongo::write_ops::InsertCommandRequest& request,
                        size_t start,
                        size_t numDocs,
                        const std::vector<size_t>& indices,
                        std::vector<mongo::write_ops::WriteError>* errors,
                        bool* containsRetry) {
    hangInsertIntoBucketCatalogBeforeCheckingTimeseriesCollection.pauseWhileSet();

    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    auto bucketsNs = write_ops_utils::makeTimeseriesBucketsNamespace(internal::ns(request));

    // Explicitly hold a reference to the CollectionCatalog, such that the corresponding
    // Collection instances remain valid, and the collator is not invalidated.
    auto catalog = CollectionCatalog::get(opCtx);
    const Collection* bucketsColl = nullptr;

    Status collectionAcquisitionStatus = Status::OK();
    TimeseriesOptions timeSeriesOptions;

    try {
        // It must be ensured that the CollectionShardingState remains consistent while rebuilding
        // the timeseriesOptions. However, the associated collection must be acquired before
        // we check for the presence of buckets collection. This ensures that a potential
        // ShardVersion mismatch can be detected, before checking for other errors.
        const auto coll = acquireCollection(opCtx,
                                            CollectionAcquisitionRequest::fromOpCtx(
                                                opCtx, bucketsNs, AcquisitionPrerequisites::kRead),
                                            MODE_IS);
        bucketsColl = catalog->lookupCollectionByNamespace(opCtx, bucketsNs);
        // Check for the presence of the buckets collection
        timeseries::assertTimeseriesBucketsCollection(bucketsColl);
        // Process timeSeriesOptions
        timeSeriesOptions = *bucketsColl->getTimeseriesOptions();
        rebuildOptionsWithGranularityFromConfigServer(opCtx, bucketsNs, timeSeriesOptions);
    } catch (const DBException& ex) {
        if (ex.code() != ErrorCodes::StaleDbVersion && !ErrorCodes::isStaleShardVersionError(ex)) {
            throw;
        }

        collectionAcquisitionStatus = ex.toStatus();

        auto& oss{OperationShardingState::get(opCtx)};
        oss.setShardingOperationFailedStatus(ex.toStatus());
    }

    TimeseriesBatches batches;
    TimeseriesStmtIds stmtIds;

    std::function<bool(size_t)> attachCollectionAcquisitionError = [&](size_t index) {
        invariant(start + index < request.getDocuments().size());
        const auto error{write_ops_exec::generateError(
            opCtx, collectionAcquisitionStatus, start + index, errors->size())};
        errors->emplace_back(std::move(*error));
        return false;
    };

    std::function<bool(size_t)> insert = [&](size_t index) {
        invariant(collectionAcquisitionStatus);
        invariant(start + index < request.getDocuments().size());

        auto stmtId = request.getStmtIds() ? request.getStmtIds()->at(start + index)
                                           : request.getStmtId().value_or(0) + start + index;

        if (isTimeseriesWriteRetryable(opCtx) &&
            TransactionParticipant::get(opCtx).checkStatementExecuted(opCtx, stmtId)) {
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
            *containsRetry = true;
            return true;
        }

        auto viewNs = internal::ns(request).isTimeseriesBucketsCollection()
            ? internal::ns(request).getTimeseriesViewNamespace()
            : internal::ns(request);
        auto& measurementDoc = request.getDocuments()[start + index];

        // It is a layering violation to have the bucket catalog be privy to the details of writing
        // out buckets with write_ops_exec functions. This callable function is a workaround in the
        // case of an uncompressed bucket that gets reopened. The bucket catalog can blindly call
        // this function handed down from write_ops_exec to write out the bucket as compressed.
        // Another option considered was to throw a write error and catch at this layer, but that
        // approach has performance implications.
        timeseries::CompressAndWriteBucketFunc compressAndWriteBucketFunc =
            compressUncompressedBucketOnReopen;

        auto swResult =
            timeseries::attemptInsertIntoBucket(opCtx,
                                                bucketCatalog,
                                                bucketsColl,
                                                timeSeriesOptions,
                                                measurementDoc,
                                                timeseries::BucketReopeningPermittance::kAllowed,
                                                compressAndWriteBucketFunc);

        if (auto error = write_ops_exec::generateError(
                opCtx, swResult.getStatus(), start + index, errors->size())) {
            invariant(swResult.getStatus().code() != ErrorCodes::WriteConflict);
            errors->emplace_back(std::move(*error));
            return false;
        }

        auto& result = swResult.getValue();
        auto* insertResult = get_if<bucket_catalog::SuccessfulInsertion>(&result);
        invariant(insertResult);
        batches.emplace_back(std::move(insertResult->batch), index);
        const auto& batch = batches.back().first;
        if (isTimeseriesWriteRetryable(opCtx)) {
            stmtIds[batch.get()].push_back(stmtId);
        }

        return true;
    };

    auto insertOrErrorFn =
        collectionAcquisitionStatus.isOK() ? insert : attachCollectionAcquisitionError;
    boost::optional<UUID> bucketsCollUUID =
        bucketsColl ? boost::make_optional(bucketsColl->uuid()) : boost::none;

    if (!indices.empty()) {
        std::for_each(indices.begin(), indices.end(), insertOrErrorFn);
    } else {
        for (size_t i = 0; i < numDocs; i++) {
            if (!insertOrErrorFn(i) && request.getOrdered()) {
                return {bucketsCollUUID, std::move(batches), std::move(stmtIds), i};
            }
        }
    }

    return {bucketsCollUUID, std::move(batches), std::move(stmtIds), request.getDocuments().size()};
}

bool commitTimeseriesBucketsAtomically(OperationContext* opCtx,
                                       const mongo::write_ops::InsertCommandRequest& request,
                                       TimeseriesBatches& batches,
                                       TimeseriesStmtIds&& stmtIds,
                                       boost::optional<repl::OpTime>* opTime,
                                       boost::optional<OID>* electionId) {
    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());

    auto batchesToCommit = timeseries::determineBatchesToCommit(batches, extractFromPair);
    if (batchesToCommit.empty()) {
        return true;
    }

    Status abortStatus = Status::OK();
    ScopeGuard batchGuard{[&] {
        for (auto batch : batchesToCommit) {
            if (batch.get()) {
                abort(bucketCatalog, batch, abortStatus);
            }
        }
    }};

    try {
        std::vector<mongo::write_ops::InsertCommandRequest> insertOps;
        std::vector<mongo::write_ops::UpdateCommandRequest> updateOps;
        auto catalog = CollectionCatalog::get(opCtx);
        auto nss = write_ops_utils::makeTimeseriesBucketsNamespace(internal::ns(request));
        auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, nss);
        assertTimeseriesBucketsCollection(bucketsColl);
        for (auto batch : batchesToCommit) {
            auto metadata = getMetadata(bucketCatalog, batch.get()->bucketId);
            auto prepareCommitStatus = bucket_catalog::prepareCommit(
                bucketCatalog, batch, bucketsColl->getDefaultCollator());
            if (!prepareCommitStatus.isOK()) {
                abortStatus = prepareCommitStatus;
                return false;
            }

            write_ops_utils::makeWriteRequest(
                opCtx, batch, metadata, stmtIds, nss, &insertOps, &updateOps);
        }

        auto result = internal::performAtomicTimeseriesWrites(opCtx, insertOps, updateOps);

        if (!result.isOK()) {
            if (result.code() == ErrorCodes::DuplicateKey) {
                bucket_catalog::resetBucketOIDCounter();
            }
            abortStatus = result;
            return false;
        }

        timeseries::getOpTimeAndElectionId(opCtx, opTime, electionId);

        for (auto batch : batchesToCommit) {
            bucket_catalog::finish(bucketCatalog, batch);
            batch.get().reset();
        }
    } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>& ex) {
        bucket_catalog::freeze(
            bucketCatalog,
            bucket_catalog::BucketId{ex->collectionUUID(), ex->bucketId(), ex->keySignature()});
        LOGV2_WARNING(
            8793900,
            "Encountered corrupt bucket while performing insert, will retry on a new bucket",
            "bucketId"_attr = ex->bucketId());
        abortStatus = ex.toStatus();
        return false;
    } catch (...) {
        abortStatus = exceptionToStatus();
        throw;
    }

    batchGuard.dismiss();
    return true;
}

/**
 * Aborts unprocessed batches, sets each user insert's error if there is any, and populates the list
 * of documents to retry.
 */
void processBatchResults(OperationContext* opCtx,
                         const TimeseriesBatches& batches,
                         std::vector<mongo::write_ops::WriteError>* errors,
                         std::vector<size_t>* docsToRetry) {
    boost::optional<mongo::write_ops::WriteError> lastError;
    if (!errors->empty()) {
        lastError = errors->back();
    }

    for (int64_t itr = 0, size = batches.size(); itr < size; ++itr) {
        const auto& [batch, index] = batches[itr];
        if (!batch) {
            continue;
        }

        // If there are any unprocessed batches, we mark them as error with the last known error.
        if (!bucket_catalog::isWriteBatchFinished(*batch)) {
            auto& bucketCatalog =
                bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
            bucket_catalog::abort(bucketCatalog, batch, lastError->getStatus());
            errors->emplace_back(index, lastError->getStatus());
            continue;
        }

        auto batchStatus = bucket_catalog::getWriteBatchStatus(*batch);
        if (batchStatus == ErrorCodes::TimeseriesBucketCleared) {
            docsToRetry->push_back(index);
            continue;
        }
        if (batchStatus == ErrorCodes::WriteConflict ||
            batchStatus == ErrorCodes::TemporarilyUnavailable) {
            docsToRetry->push_back(index);
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            continue;
        }
        if (auto error = write_ops_exec::generateError(opCtx, batchStatus, index, errors->size())) {
            errors->emplace_back(std::move(*error));
            continue;
        }
    }
}

/**
 * Writes to the underlying system.buckets collection as a series of ordered time-series inserts.
 * Returns true on success, false otherwise, filling out errors as appropriate on failure as well
 * as containsRetry which is used at a higher layer to report a retry count metric.
 */
bool performOrderedTimeseriesWritesAtomically(OperationContext* opCtx,
                                              const mongo::write_ops::InsertCommandRequest& request,
                                              std::vector<mongo::write_ops::WriteError>* errors,
                                              boost::optional<repl::OpTime>* opTime,
                                              boost::optional<OID>* electionId,
                                              bool* containsRetry) {
    auto [_, batches, stmtIds, numInserted] = insertIntoBucketCatalog(
        opCtx, request, 0, request.getDocuments().size(), {}, errors, containsRetry);

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    if (!commitTimeseriesBucketsAtomically(
            opCtx, request, batches, std::move(stmtIds), opTime, electionId)) {
        return false;
    }

    return true;
}

/**
 * Writes to the underlying system.buckets collection. Returns the indices, of the batch
 * which were attempted in an update operation, but found no bucket to update. These indices
 * can be passed as the 'indices' parameter in a subsequent call to this function, in order
 * to to be retried.
 * In rare cases due to collision from OID generation, we will also retry inserting those bucket
 * documents for a limited number of times.
 */
std::vector<size_t> performUnorderedTimeseriesWrites(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    size_t start,
    size_t numDocs,
    const std::vector<size_t>& indices,
    std::vector<mongo::write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry,
    absl::flat_hash_map<int, int>& retryAttemptsForDup) {
    auto [optUuid, batches, bucketStmtIds, _] =
        insertIntoBucketCatalog(opCtx, request, start, numDocs, indices, errors, containsRetry);

    tassert(9213700,
            "Timeseries insert did not find bucket collection UUID, but staged inserts in "
            "the in-memory bucket catalog.",
            optUuid || batches.empty());

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    if (batches.empty()) {
        return {};
    }

    bool canContinue = true;
    std::vector<size_t> docsToRetry;

    UUID collectionUUID = *optUuid;
    size_t itr = 0;
    stdx::unordered_set<bucket_catalog::WriteBatch*> processedBatches;
    for (; itr < batches.size() && canContinue; ++itr) {
        auto& [batch, index] = batches[itr];
        if (!processedBatches.contains(batch.get())) {
            processedBatches.insert(batch.get());
            auto stmtIds = isTimeseriesWriteRetryable(opCtx) ? std::move(bucketStmtIds[batch.get()])
                                                             : std::vector<StmtId>{};
            try {
                canContinue = internal::commitTimeseriesBucket(opCtx,
                                                               batch,
                                                               start,
                                                               index,
                                                               std::move(stmtIds),
                                                               errors,
                                                               opTime,
                                                               electionId,
                                                               &docsToRetry,
                                                               retryAttemptsForDup,
                                                               request);
            } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>& ex) {
                auto bucketId = ex.extraInfo<timeseries::BucketCompressionFailure>()->bucketId();
                auto keySignature =
                    ex.extraInfo<timeseries::BucketCompressionFailure>()->keySignature();

                bucket_catalog::freeze(
                    bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext()),
                    bucket_catalog::BucketId{collectionUUID, bucketId, keySignature});

                LOGV2_WARNING(
                    8607200,
                    "Failed to compress bucket for time-series insert, please retry your write",
                    "bucketId"_attr = bucketId);

                errors->emplace_back(*write_ops_exec::generateError(
                    opCtx, ex.toStatus(), start + index, errors->size()));
            }

            batch.reset();
        }
    }

    processBatchResults(opCtx, batches, errors, &docsToRetry);
    // If we cannot continue the request, we should convert all the 'docsToRetry' into an error.
    if (!canContinue) {
        invariant(!errors->empty());
        for (auto&& index : docsToRetry) {
            errors->emplace_back(index, errors->back().getStatus());
        }
        docsToRetry.clear();
    }
    return docsToRetry;
}

}  // namespace

namespace internal {

NamespaceString ns(const mongo::write_ops::InsertCommandRequest& request) {
    return request.getNamespace();
}

Status performAtomicTimeseriesWrites(
    OperationContext* opCtx,
    const std::vector<mongo::write_ops::InsertCommandRequest>& insertOps,
    const std::vector<mongo::write_ops::UpdateCommandRequest>& updateOps) try {
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    invariant(!opCtx->inMultiDocumentTransaction());
    invariant(!insertOps.empty() || !updateOps.empty());
    auto expectedUUID = !insertOps.empty() ? insertOps.front().getCollectionUUID()
                                           : updateOps.front().getCollectionUUID();
    invariant(expectedUUID.has_value());

    auto ns =
        !insertOps.empty() ? insertOps.front().getNamespace() : updateOps.front().getNamespace();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    write_ops_exec::LastOpFixer lastOpFixer(opCtx);
    lastOpFixer.startingOp(ns);

    const auto coll =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx, ns, AcquisitionPrerequisites::kWrite, expectedUUID),
                          MODE_IX);
    if (!coll.exists()) {
        write_ops::assertTimeseriesBucketsCollectionNotFound(ns);
    }

    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                   .getDatabaseProfileLevel(ns.dbName()));

    mongo::write_ops_exec::assertCanWrite_inlock(opCtx, ns);

    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const bool replicateVectoredInsertsTransactionally = fcvSnapshot.isVersionInitialized() &&
        repl::feature_flags::gReplicateVectoredInsertsTransactionally.isEnabled(fcvSnapshot);

    WriteUnitOfWork::OplogEntryGroupType oplogEntryGroupType = WriteUnitOfWork::kDontGroup;
    if (replicateVectoredInsertsTransactionally && insertOps.size() > 1 && updateOps.empty() &&
        !repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, ns)) {
        oplogEntryGroupType = WriteUnitOfWork::kGroupForPossiblyRetryableOperations;
    }
    WriteUnitOfWork wuow{opCtx, oplogEntryGroupType};

    std::vector<repl::OpTime> oplogSlots;
    boost::optional<std::vector<repl::OpTime>::iterator> slot;
    if (!replicateVectoredInsertsTransactionally || !updateOps.empty()) {
        if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, ns)) {
            oplogSlots = repl::getNextOpTimes(opCtx, insertOps.size() + updateOps.size());
            slot = oplogSlots.begin();
        }
    }

    auto participant = TransactionParticipant::get(opCtx);
    // Since we are manually updating the "lastWriteOpTime" before committing, we'll also need to
    // manually reset if the storage transaction is aborted.
    if (slot && participant) {
        shard_role_details::getRecoveryUnit(opCtx)->onRollback([](OperationContext* opCtx) {
            TransactionParticipant::get(opCtx).setLastWriteOpTime(opCtx, repl::OpTime());
        });
    }

    std::vector<InsertStatement> inserts;
    inserts.reserve(insertOps.size());

    for (auto& op : insertOps) {
        invariant(op.getDocuments().size() == 1);

        inserts.emplace_back(op.getStmtIds() ? *op.getStmtIds()
                                             : std::vector<StmtId>{kUninitializedStmtId},
                             op.getDocuments().front(),
                             slot ? *(*slot)++ : OplogSlot{});
    }

    if (!insertOps.empty()) {
        auto status = collection_internal::insertDocuments(
            opCtx, coll.getCollectionPtr(), inserts.begin(), inserts.end(), &curOp->debug());
        if (!status.isOK()) {
            return status;
        }
        if (slot && participant) {
            // Manually sets the timestamp so that the "prevOpTime" field in the oplog entry is
            // correctly chained to the previous operations.
            participant.setLastWriteOpTime(opCtx, *(std::prev(*slot)));
        }
    }

    for (auto& op : updateOps) {
        invariant(op.getUpdates().size() == 1);
        auto& update = op.getUpdates().front();

        invariant(coll.getCollectionPtr()->isClustered());
        auto recordId = record_id_helpers::keyForOID(update.getQ()["_id"].OID());

        auto original = coll.getCollectionPtr()->docFor(opCtx, recordId);

        CollectionUpdateArgs args{original.value()};
        args.criteria = update.getQ();
        if (const auto& stmtIds = op.getStmtIds()) {
            args.stmtIds = *stmtIds;
        }
        args.source = OperationSource::kTimeseriesInsert;

        BSONObj updated;
        BSONObj diffFromUpdate;
        const BSONObj* diffOnIndexes =
            collection_internal::kUpdateAllIndexes;  // Assume all indexes are affected.
        if (update.getU().type() == mongo::write_ops::UpdateModification::Type::kDelta) {
            diffFromUpdate = update.getU().getDiff();
            updated = doc_diff::applyDiff(original.value(),
                                          diffFromUpdate,
                                          update.getU().mustCheckExistenceForInsertOperations(),
                                          update.getU().verifierFunction);
            diffOnIndexes = &diffFromUpdate;
            args.update = update_oplog_entry::makeDeltaOplogEntry(diffFromUpdate);
        } else if (update.getU().type() == mongo::write_ops::UpdateModification::Type::kTransform) {
            const auto& transform = update.getU().getTransform();
            auto transformed = transform(original.value());
            tassert(7050400,
                    "Could not apply transformation to time series bucket document",
                    transformed.has_value());
            updated = std::move(transformed.value());
            args.update = update_oplog_entry::makeReplacementOplogEntry(updated);
        } else {
            invariant(false, "Unexpected update type");
        }

        if (slot) {
            args.oplogSlots = {**slot};
            fassert(5481600,
                    shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(
                        args.oplogSlots[0].getTimestamp()));
        }

        collection_internal::updateDocument(opCtx,
                                            coll.getCollectionPtr(),
                                            recordId,
                                            original,
                                            updated,
                                            diffOnIndexes,
                                            nullptr /*indexesAffected*/,
                                            &curOp->debug(),
                                            &args);
        if (slot) {
            if (participant) {
                // Manually sets the timestamp so that the "prevOpTime" field in the oplog entry is
                // correctly chained to the previous operations.
                participant.setLastWriteOpTime(opCtx, **slot);
            }
            ++(*slot);
        }
    }

    if (MONGO_unlikely(failAtomicTimeseriesWrites.shouldFail())) {
        return {ErrorCodes::FailPointEnabled,
                "Failing time-series writes due to failAtomicTimeseriesWrites fail point"};
    }

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();

    return Status::OK();
} catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>&) {
    // If we encounter a TimeseriesBucketCompressionFailure, we should throw to
    // a higher level (write_ops_exec::performUpdates) so that we can freeze the corrupt bucket.
    throw;
} catch (const DBException& ex) {
    return ex.toStatus();
}

bool commitTimeseriesBucket(OperationContext* opCtx,
                            std::shared_ptr<bucket_catalog::WriteBatch> batch,
                            size_t start,
                            size_t index,
                            std::vector<StmtId>&& stmtIds,
                            std::vector<mongo::write_ops::WriteError>* errors,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId,
                            std::vector<size_t>* docsToRetry,
                            absl::flat_hash_map<int, int>& retryAttemptsForDup,
                            const mongo::write_ops::InsertCommandRequest& request) try {
    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());

    auto metadata = getMetadata(bucketCatalog, batch->bucketId);
    auto catalog = CollectionCatalog::get(opCtx);
    auto nss = write_ops_utils::makeTimeseriesBucketsNamespace(internal::ns(request));
    auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, nss);
    assertTimeseriesBucketsCollection(bucketsColl);
    auto status = prepareCommit(bucketCatalog, batch, bucketsColl->getDefaultCollator());
    if (!status.isOK()) {
        invariant(bucket_catalog::isWriteBatchFinished(*batch));
        docsToRetry->push_back(index);
        return true;
    }

    const auto docId = batch->bucketId.oid;
    const bool performInsert = batch->numPreviouslyCommittedMeasurements == 0;
    if (performInsert) {
        const auto output =
            performTimeseriesInsert(opCtx, metadata, std::move(stmtIds), request, batch);
        if (auto error = write_ops_exec::generateError(
                opCtx, output.result.getStatus(), start + index, errors->size())) {
            bool canContinue = output.canContinue;
            // Automatically attempts to retry on DuplicateKey error.
            if (error->getStatus().code() == ErrorCodes::DuplicateKey &&
                retryAttemptsForDup[index]++ < gTimeseriesInsertMaxRetriesOnDuplicates.load()) {
                docsToRetry->push_back(index);
                canContinue = true;
            } else {
                errors->emplace_back(std::move(*error));
            }
            abort(bucketCatalog, batch, output.result.getStatus());
            return canContinue;
        }

        invariant(output.result.getValue().getN() == 1,
                  str::stream() << "Expected 1 insertion of document with _id '" << docId
                                << "', but found " << output.result.getValue().getN() << ".");
    } else {
        auto op = write_ops_utils::makeTimeseriesCompressedDiffUpdateOp(
            opCtx, batch, nss, std::move(stmtIds));

        auto const output = performTimeseriesUpdate(opCtx, metadata, op, request);

        if ((output.result.isOK() && output.result.getValue().getNModified() != 1) ||
            output.result.getStatus().code() == ErrorCodes::WriteConflict ||
            output.result.getStatus().code() == ErrorCodes::TemporarilyUnavailable) {
            abort(bucketCatalog,
                  batch,
                  output.result.isOK()
                      ? Status{ErrorCodes::WriteConflict, "Could not update non-existent bucket"}
                      : output.result.getStatus());
            docsToRetry->push_back(index);
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            return true;
        } else if (auto error = write_ops_exec::generateError(
                       opCtx, output.result.getStatus(), start + index, errors->size())) {
            errors->emplace_back(std::move(*error));
            abort(bucketCatalog, batch, output.result.getStatus());
            return output.canContinue;
        }
    }

    timeseries::getOpTimeAndElectionId(opCtx, opTime, electionId);

    bucket_catalog::finish(bucketCatalog, batch);
    return true;
} catch (const DBException& ex) {
    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    abort(bucketCatalog, batch, ex.toStatus());
    throw;
}

void performUnorderedTimeseriesWritesWithRetries(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    size_t start,
    size_t numDocs,
    std::vector<mongo::write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry) {
    std::vector<size_t> docsToRetry;
    absl::flat_hash_map<int, int> retryAttemptsForDup;
    do {
        docsToRetry = performUnorderedTimeseriesWrites(opCtx,
                                                       request,
                                                       start,
                                                       numDocs,
                                                       docsToRetry,
                                                       errors,
                                                       opTime,
                                                       electionId,
                                                       containsRetry,
                                                       retryAttemptsForDup);
        if (!retryAttemptsForDup.empty()) {
            bucket_catalog::resetBucketOIDCounter();
        }
    } while (!docsToRetry.empty());
}

size_t performOrderedTimeseriesWrites(OperationContext* opCtx,
                                      const mongo::write_ops::InsertCommandRequest& request,
                                      std::vector<mongo::write_ops::WriteError>* errors,
                                      boost::optional<repl::OpTime>* opTime,
                                      boost::optional<OID>* electionId,
                                      bool* containsRetry) {
    if (performOrderedTimeseriesWritesAtomically(
            opCtx, request, errors, opTime, electionId, containsRetry)) {
        if (!errors->empty()) {
            invariant(errors->size() == 1);
            return errors->front().getIndex();
        }
        return request.getDocuments().size();
    }

    for (size_t i = 0; i < request.getDocuments().size(); ++i) {
        performUnorderedTimeseriesWritesWithRetries(
            opCtx, request, i, 1, errors, opTime, electionId, containsRetry);
        if (!errors->empty()) {
            return i;
        }
    }

    return request.getDocuments().size();
}

StatusWith<std::vector<bucket_catalog::BatchedInsertContext>> buildBatchedInsertContextsNoMetaField(
    const bucket_catalog::BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    bucket_catalog::ExecutionStatsController& stats,
    tracking::Context& trackingContext) {

    std::vector<bucket_catalog::BatchedInsertTuple> batchedInsertTupleVector;

    // As part of the InsertBatchTuple struct we store the index of the measurement in the original
    // user batch for error reporting and retryability purposes.
    for (size_t i = 0; i < userMeasurementsBatch.size(); i++) {
        auto swTime =
            bucket_catalog::extractTime(userMeasurementsBatch[i], timeseriesOptions.getTimeField());
        if (!swTime.isOK()) {
            return swTime.getStatus();
        }
        batchedInsertTupleVector.emplace_back(userMeasurementsBatch[i], swTime.getValue(), i);
    }

    // Empty metadata.
    BSONElement metadata;
    auto bucketKey = bucket_catalog::BucketKey{
        collectionUUID, bucket_catalog::BucketMetadata{trackingContext, metadata, boost::none}};
    auto stripeNumber = bucket_catalog::internal::getStripeNumber(bucketCatalog, bucketKey);

    std::sort(
        batchedInsertTupleVector.begin(), batchedInsertTupleVector.end(), [](auto& lhs, auto& rhs) {
            // Sort measurements on their timeField.
            return std::get<Date_t>(lhs) < std::get<Date_t>(rhs);
        });

    return {{bucket_catalog::BatchedInsertContext(
        bucketKey, stripeNumber, timeseriesOptions, stats, batchedInsertTupleVector)}};
};

StatusWith<std::vector<bucket_catalog::BatchedInsertContext>>
buildBatchedInsertContextsWithMetaField(const bucket_catalog::BucketCatalog& bucketCatalog,
                                        const UUID& collectionUUID,
                                        const TimeseriesOptions& timeseriesOptions,
                                        const std::vector<BSONObj>& userMeasurementsBatch,
                                        StringData metaFieldName,
                                        bucket_catalog::ExecutionStatsController& stats,
                                        tracking::Context& trackingContext) {
    // Maps from the string representation of a distinct metaField value to a vector of
    // BatchedInsertTuples whose measurements have that same metaField value.
    stdx::unordered_map<std::string, std::vector<bucket_catalog::BatchedInsertTuple>>
        metaFieldToBatchedInsertTuples;
    // Maps from the string representation of a metaField value to the BSONElement of that metaField
    // value. Workaround for the fact that BSONElements are not hashable.
    stdx::unordered_map<std::string, BSONElement> metaFieldStringToBSONElement;

    // Go through the vector of user measurements and create a map from each distinct metaField
    // value to a vector of InsertBatchTuples for that metaField. As part of the InsertBatchTuple
    // struct we store the index of the measurement in the original user batch for error reporting
    // and retryability purposes.
    for (size_t i = 0; i < userMeasurementsBatch.size(); i++) {
        auto swTimeAndMeta =
            bucket_catalog::extractTimeAndMeta(userMeasurementsBatch[i],
                                               timeseriesOptions.getTimeField(),
                                               timeseriesOptions.getMetaField().get());
        if (!swTimeAndMeta.isOK()) {
            return swTimeAndMeta.getStatus();
        }
        auto time = std::get<Date_t>(swTimeAndMeta.getValue());
        auto meta = std::get<BSONElement>(swTimeAndMeta.getValue());

        metaFieldStringToBSONElement.try_emplace(meta.String(), meta);
        metaFieldToBatchedInsertTuples.try_emplace(
            meta.String(), std::vector<bucket_catalog::BatchedInsertTuple>{});

        metaFieldToBatchedInsertTuples[meta.String()].emplace_back(
            userMeasurementsBatch[i], time, i);
    }

    std::vector<bucket_catalog::BatchedInsertContext> batchedInsertContexts;

    // Go through all meta-unique batches, sort by time, and fill result
    for (auto& [metaFieldString, batchedInsertTupleVector] : metaFieldToBatchedInsertTuples) {
        std::sort(batchedInsertTupleVector.begin(),
                  batchedInsertTupleVector.end(),
                  [](auto& lhs, auto& rhs) {
                      // Sort measurements on their timeField.
                      return std::get<Date_t>(lhs) < std::get<Date_t>(rhs);
                  });
        auto metadata = metaFieldStringToBSONElement[metaFieldString];
        auto bucketKey = bucket_catalog::BucketKey{
            collectionUUID, bucket_catalog::BucketMetadata{trackingContext, metadata, boost::none}};
        auto stripeNumber = bucket_catalog::internal::getStripeNumber(bucketCatalog, bucketKey);

        batchedInsertContexts.emplace_back(bucket_catalog::BatchedInsertContext(
            bucketKey, stripeNumber, timeseriesOptions, stats, batchedInsertTupleVector));
    }

    return batchedInsertContexts;
}

StatusWith<std::vector<bucket_catalog::BatchedInsertContext>> buildBatchedInsertContexts(
    bucket_catalog::BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch) {

    auto metaFieldName = timeseriesOptions.getMetaField();
    auto& trackingContext = bucket_catalog::getTrackingContext(
        bucketCatalog.trackingContexts, bucket_catalog::TrackingScope::kOpenBucketsByKey);
    auto stats =
        bucket_catalog::internal::getOrInitializeExecutionStats(bucketCatalog, collectionUUID);

    auto swBatchedInsertContexts = (metaFieldName)
        ? buildBatchedInsertContextsWithMetaField(bucketCatalog,
                                                  collectionUUID,
                                                  timeseriesOptions,
                                                  userMeasurementsBatch,
                                                  metaFieldName.get(),
                                                  stats,
                                                  trackingContext)
        : buildBatchedInsertContextsNoMetaField(bucketCatalog,
                                                collectionUUID,
                                                timeseriesOptions,
                                                userMeasurementsBatch,
                                                stats,
                                                trackingContext);

    if (!swBatchedInsertContexts.isOK()) {
        return swBatchedInsertContexts.getStatus();
    }

    return swBatchedInsertContexts.getValue();
}

}  // namespace internal

}  // namespace mongo::timeseries::write_ops
