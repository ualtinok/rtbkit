/** post_auction_service.h                                 -*- C++ -*-
    Rémi Attab, 18 Apr 2014
    Copyright (c) 2014 Datacratic.  All rights reserved.

    Post auction service that matches bids to win and campaign events.

*/

#pragma once

#include "event_matcher.h"
#include "rtbkit/core/monitor/monitor_provider.h"
#include "rtbkit/core/agent_configuration/agent_configuration_listener.h"
#include "soa/service/logs.h"
#include "soa/service/service_base.h"
#include "soa/service/loop_monitor.h"
#include "soa/service/zmq_endpoint.h"
#include "soa/service/zmq_named_pub_sub.h"
#include "soa/service/zmq_message_router.h"

namespace RTBKIT {

/******************************************************************************/
/* POST AUCTION SERVICE                                                       */
/******************************************************************************/

struct PostAuctionService : public ServiceBase, public MonitorProvider
{
    PostAuctionService(ServiceBase & parent,
                    const std::string & serviceName);
    PostAuctionService(std::shared_ptr<ServiceProxies> proxies,
                    const std::string & serviceName);


    ~PostAuctionService() { shutdown(); }


    void init();
    void start(std::function<void ()> onStop = std::function<void ()>());
    void shutdown();

    /// Start listening on ports for connections from agents, routers
    /// and event sources
    void bindTcp();


    /************************************************************************/
    /* BANKER                                                               */
    /************************************************************************/

    std::shared_ptr<Banker> getBanker() const
    {
        return banker;
    }

    void setBanker(const std::shared_ptr<Banker> & newBanker)
    {
        matcher.setBanker(banker = newBanker);
    }


    /**************************************************************************/
    /* TIMEOUTS                                                               */
    /**************************************************************************/

    void setWinTimeout(const float & timeOut) {

        if (timeOut < 0.0)
            throw ML::Exception("Invalid timeout for Win timeout");

        matcher.setWinTimeout(winTimeout = timeOut);
    }

    void setAuctionTimeout(const float & timeOut) {

        if (timeOut < 0.0)
            throw ML::Exception("Invalid timeout for Win timeout");

        matcher.setWinTimeout(auctionTimeout = timeOut);
    }


    /************************************************************************/
    /* LOGGING                                                              */
    /************************************************************************/

    /** Log a given message to the given channel. */
    template<typename... Args>
    void logMessage(const std::string & channel, Args... args)
    {
        logger.publish(channel, Date::now().print(5), args...);
    }

    /** Log a router error. */
    template<typename... Args>
    void logPAError(const std::string & function,
                    const std::string & exception,
                    Args... args)
    {
        logger.publish("PAERROR",
                Date::now().print(5), function, exception, args...);
        recordHit("error.%s", function);
    }


    /************************************************************************/
    /* EVENT MATCHING                                                       */
    /************************************************************************/

    /** Transfer the given auction to the post auction loop.  This method
        assumes that the given auction was submitted with a non-empty
        bid, and adds it to the internal data structures so that any
        post-auction messages can be matched up with it.
    */
    void injectSubmittedAuction(
            const Id & auctionId,
            const Id & adSpotId,
            std::shared_ptr<BidRequest> bidRequest,
            const std::string & bidRequestStr,
            const std::string & bidRequestStrFormat,
            const JsonHolder & augmentations,
            const Auction::Response & bidResponse,
            Date lossTimeout);

    /** Inject a WIN into the post auction loop.  Thread safe and
        asynchronous. */
    void injectWin(
            const Id & auctionId,
            const Id & adspot,
            Amount winPrice,
            Date timestamp,
            const JsonHolder & winMeta,
            const UserIds & ids,
            const AccountKey & account,
            Date bidTimestamp);

    /** Inject a LOSS into the router.  Thread safe and asynchronous.
        Note that this method ONLY is useful for simulations; otherwise
        losses are implicit.
    */
    void injectLoss(
            const Id & auctionId,
            const Id & adspot,
            Date timestamp,
            const JsonHolder & lossMeta,
            const AccountKey & account,
            Date bidTimestamp);

    /** Inject a campaign event into the router, to be passed on to the agent
        that bid on it.

        If the spot ID is empty, then the click will be sent to all agents
        that had a win on the auction.
    */
    void injectCampaignEvent(
            const std::string & label,
            const Id & auctionId,
            const Id & adSpotId,
            Date timestamp,
            const JsonHolder & eventMeta,
            const UserIds & ids);

private:

    std::string getProviderClass() const;
    MonitorIndicator getProviderIndicators() const;


    /** Initialize all of our connections, hooking everything in to the
        event loop.
    */
    void initConnections();

    void doAuction(const SubmittedAuctionEvent & event);
    void doEvent(const std::shared_ptr<PostAuctionEvent> & event);
    void doCampaignEvent(const std::shared_ptr<PostAuctionEvent> & event);
    void checkExpiredAuctions();

    /** Decode from zeromq and handle a new auction that came in. */
    void doAuctionMessage(const std::vector<std::string> & message);

    /** Decode from zeromq and handle a new auction that came in. */
    void doWinMessage(const std::vector<std::string> & message);

    /** Decode from zeromq and handle a new auction that came in. */
    void doLossMessage(const std::vector<std::string> & message);

    /** Decode from zeromq and handle a new campaign event message that came
     * in. */
    void doCampaignEventMessage(const std::vector<std::string> & message);

    void doConfigChange(
            const std::string & agent,
            std::shared_ptr<const AgentConfig> config);


    void doMatchedWinLoss(MatchedWinLoss event);
    void doMatchedCampaignEvent(MatchedCampaignEvent event);
    void doUnmatched(UnmatchedEvent event);
    void doError(PostAuctionErrorEvent error);


    /** Send out a post-auction event to anything that may be listening. */
    bool routePostAuctionEvent(
            const std::string & label,
            const FinishedInfo & finished,
            const SegmentList & channels,
            bool filterChannels);

    /** Send the given message to the given bidding agent. */
    template<typename... Args>
    void sendAgentMessage(const std::string & agent,
                          const std::string & messageType,
                          const Date & date,
                          Args... args)
    {
        toAgents.sendMessage(agent, messageType, date,
                             std::forward<Args>(args)...);
    }

    /** Send the given message to the given bidding agent. */
    template<typename... Args>
    void sendAgentMessage(const std::string & agent,
                          const std::string & eventType,
                          const std::string & messageType,
                          const Date & date,
                          Args... args)
    {
        toAgents.sendMessage(agent, eventType, messageType, date,
                             std::forward<Args>(args)...);
    }


    float auctionTimeout;
    float winTimeout;

    Date lastWinLoss;
    Date lastCampaignEvent;

    MessageLoop loop;
    LoopMonitor loopMonitor;

    EventMatcher matcher;
    std::shared_ptr<Banker> banker;
    AgentConfigurationListener configListener;
    MonitorProviderClient monitorProviderClient;

    TypedMessageSink<SubmittedAuctionEvent> auctions;
    TypedMessageSink<std::shared_ptr<PostAuctionEvent> > events;

    ZmqNamedPublisher logger;
    ZmqNamedEndpoint endpoint;
    ZmqNamedClientBus toAgents;

    ZmqMessageRouter router;

    static Logging::Category print;
    static Logging::Category error;
    static Logging::Category trace;
};

} // namespace RTBKIT
