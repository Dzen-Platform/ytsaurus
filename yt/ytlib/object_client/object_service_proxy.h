#pragma once

#include "public.h"

#include <core/misc/nullable.h>

#include <core/ytree/ypath_client.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/object_client/object_service.pb.h>

namespace NYT {
namespace NObjectClient {

////////////////////////////////////////////////////////////////////////////////

class TObjectServiceProxy
    : public NRpc::TProxyBase
{
public:
    static Stroka GetServiceName();
    static int GetProtocolVersion();

    explicit TObjectServiceProxy(NRpc::IChannelPtr channel);

    DEFINE_RPC_PROXY_METHOD(NProto, Execute);
    DEFINE_RPC_PROXY_METHOD(NProto, GCCollect);

    //! Executes a single typed request.
    template <class TTypedRequest>
    TFuture< TIntrusivePtr<typename TTypedRequest::TTypedResponse> >
    Execute(TIntrusivePtr<TTypedRequest> innerRequest);

    class TReqExecuteBatch;
    class TRspExecuteBatch;

    //! Mimics the types introduced by |DEFINE_RPC_PROXY_METHOD|.
    typedef TIntrusivePtr<TReqExecuteBatch> TReqExecuteBatchPtr;
    typedef TIntrusivePtr<TRspExecuteBatch> TRspExecuteBatchPtr;
    typedef TErrorOr<TRspExecuteBatchPtr> TErrorOrRspExecuteBatchPtr;

    //! A batched request to Cypress that holds a vector of individual requests that
    //! are transferred within a single RPC envelope.
    class TReqExecuteBatch
        : public NRpc::TClientRequest
    {
    public:
        TReqExecuteBatch(
            NRpc::IChannelPtr channel,
            const Stroka& path,
            const Stroka& method);

        //! Runs asynchronous invocation.
        TFuture<TRspExecuteBatchPtr> Invoke();

        //! Overrides base method for fluent use.
        TReqExecuteBatchPtr SetTimeout(TNullable<TDuration> timeout);

        //! Adds an individual request into the batch.
        /*!
         *  Each individual request may be marked with a key.
         *  These keys can be used to retrieve the corresponding responses
         *  (thus avoiding complicated and error-prone index calculations).
         *
         *  The client is allowed to issue an empty (|NULL|) request. This request is treated
         *  like any other and it sent to the server. The server typically sends an empty (|NULL|)
         *  response back. This feature is useful for adding dummy requests to keep
         *  the request list aligned with some other data structure.
         */
        TReqExecuteBatchPtr AddRequest(
            NYTree::TYPathRequestPtr innerRequest,
            const Stroka& key = "");

        //! Similar to #AddRequest, but works for already serialized messages representing requests.
        TReqExecuteBatchPtr AddRequestMessage(
            TSharedRefArray innerRequestMessage,
            const Stroka& key = "");

        //! Returns the current number of individual requests in the batch.
        int GetSize() const;

    private:
        typedef std::multimap<Stroka, int> TKeyToIndexMultimap;

        std::vector<TSharedRefArray> InnerRequestMessages;
        TKeyToIndexMultimap KeyToIndexes;

        virtual TSharedRef SerializeBody() override;

        void OnResponse(
            TNullable<TInstant> deadline,
            TPromise<TRspExecuteBatchPtr> promise,
            TRspExecuteBatchPtr response);

    };

    //! A response to a batched request.
    /*!
     *  This class holds a vector of messages representing responses to individual
     *  requests that were earlier sent to Cypress.
     *
     *  The length of this vector (see #GetSize) coincides to that of the requests vector.
     *
     *  Individual responses can be extracted by calling #GetResponse. Since they may be of
     *  different actual types, the client must supply an additional type parameter.
     *  Responses may also be retrieved by specifying a key that was used during
     *  request insertion.
     *
     */
    class TRspExecuteBatch
        : public NRpc::TClientResponse
    {
    public:
        typedef std::multimap<Stroka, int> TKeyToIndexMultimap;

        TRspExecuteBatch(
            NRpc::TClientContextPtr clientContext,
            const TKeyToIndexMultimap& keyToIndexes);

        TPromise<TRspExecuteBatchPtr> GetPromise();

        //! Returns the number of individual responses in the batch.
        int GetSize() const;

        //! Returns the individual response with a given index.
        template <class TTypedResponse>
        TErrorOr<TIntrusivePtr<TTypedResponse>> GetResponse(int index) const;

        //! Returns the individual generic response with a given index.
        TErrorOr<NYTree::TYPathResponsePtr> GetResponse(int index) const;

        //! Returns the individual generic response with a given key or NULL if no request with
        //! this key is known. At most one such response must exist.
        TNullable<TErrorOr<NYTree::TYPathResponsePtr>> FindResponse(const Stroka& key) const;

        //! Returns the individual generic response with a given key.
        //! Such a response must be unique.
        TErrorOr<NYTree::TYPathResponsePtr> GetResponse(const Stroka& key) const;

        //! Returns the individual response with a given key or NULL if no request with
        //! this key is known. At most one such response must exist.
        template <class TTypedResponse>
        TNullable<TErrorOr<TIntrusivePtr<TTypedResponse>>> FindResponse(const Stroka& key) const;

        //! Returns the individual response with a given key.
        //! Such a response must be unique.
        template <class TTypedResponse>
        TErrorOr<TIntrusivePtr<TTypedResponse>> GetResponse(const Stroka& key) const;

        //! Returns all responses with a given key (all if no key is specified).
        template <class TTypedResponse>
        std::vector<TErrorOr<TIntrusivePtr<TTypedResponse>>> GetResponses(const Stroka& key = "") const;

        //! Returns all responses with a given key (all if no key is specified).
        std::vector<TErrorOr<NYTree::TYPathResponsePtr>> GetResponses(const Stroka& key = "") const;

        //! Similar to #GetResponse, but returns the response message without deserializing it.
        TSharedRefArray GetResponseMessage(int index) const;

    private:
        TKeyToIndexMultimap KeyToIndexes;
        TPromise<TRspExecuteBatchPtr> Promise;
        NProto::TRspExecute Body;
        std::vector<int> BeginPartIndexes;

        virtual void SetPromise(const TError& error) override;
        virtual void DeserializeBody(const TRef& data) override;

    };

    //! Executes a batched Cypress request.
    TReqExecuteBatchPtr ExecuteBatch();

};

////////////////////////////////////////////////////////////////////////////////

//! Returns the cumulative error for the whole batch.
/*!
 *  If the envelope request has fails then the corresponding error is returned.
 *  Otherwise, individual responses are examined and a cumulative error
 *  is constructed (with individual errors attached as inner).
 *  If all individual responses were successful then OK is returned.
 */
TError GetCumulativeError(const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError);

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectClient
} // namespace NYT

#define OBJECT_SERVICE_PROXY_INL_H_
#include "object_service_proxy-inl.h"
#undef OBJECT_SERVICE_PROXY_INL_H_
