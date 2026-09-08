#ifndef UWS_TOPICTREE_H
#define UWS_TOPICTREE_H

#include <map>
#include <list>
#include <iostream>
#include <unordered_set>
#include <utility>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string_view>
#include <functional>
#include <set>
#include <string>
#include <exception>
#include <algorithm>

namespace uWS {

struct Subscriber;

struct Topic : std::unordered_set<Subscriber *> {

    Topic(std::string_view topic) : name(topic) {

    }

    std::string name;
};

struct Subscriber {

    template <typename, typename> friend struct TopicTree;

private:
    /* We use a factory */
    Subscriber() = default;

    /* State of prev, next does not matter unless we are needsDrainage() since we are not in the list */
    Subscriber *prev, *next;

    /* Any one subscriber can be part of at most 32 publishes before it needs a drain,
     * or whatever encoding of runs or whatever we might do in the future */
    uint16_t messageIndices[32];

    /* This one matters the most, if it is 0 we are not in the list of drainableSubscribers */
    unsigned char numMessageIndices = 0;

    /* Set when queued for deferred destruction */
    bool pendingDestruction = false;

public:

    /* We have a list of topics we subscribe to (read by WebSocket::iterateTopics) */
    std::set<Topic *> topics;

    /* User data */
    void *user;

    bool needsDrainage() {
        return numMessageIndices;
    }
};

template <typename T, typename B>
struct TopicTree {

    enum IteratorFlags {
        LAST = 1,
        FIRST = 2
    };

    /* Whomever is iterating this topic is locked to not modify its own list */
    Subscriber *iteratingSubscriber = nullptr;

private:

    struct PendingOp {
        enum Kind : unsigned char { SUBSCRIBE, UNSUBSCRIBE, PUBLISH } kind;
        Subscriber *subscriber;   // SUBSCRIBE/UNSUBSCRIBE: the subscriber. PUBLISH: sender (may be nullptr).
        Topic *topic;             // SUBSCRIBE/UNSUBSCRIBE only; nulled once applied.
        std::string topicName;    // PUBLISH only.
        T message;               // PUBLISH only.
    };
    std::function<bool(Subscriber *, T &, IteratorFlags)> cb;
    std::unordered_map<std::string_view, std::unique_ptr<Topic>> topics;
    Subscriber *drainableSubscribers = nullptr;
    int drainingDepth = 0;
    Topic *iteratingTopic = nullptr;
    bool pending = false;
    bool applying = false;
    std::vector<Subscriber *> pendingFree;
    std::vector<PendingOp> pendingOps;
    std::vector<T> outgoingMessages;

    bool isIterating(Subscriber *s) {
        if (iteratingSubscriber == s) {
            std::cerr << "Error: WebSocket must not subscribe or unsubscribe to topics while iterating its topics!" << std::endl;
            return true;
        }
        return false;
    }

    void drainImpl(Subscriber *s) {
        /* Before we call cb we need to make sure this subscriber will not report needsDrainage()
         * since WebSocket::send will call drain from within the cb in that case.*/
        int numMessageIndices = s->numMessageIndices;
        s->numMessageIndices = 0;

        /* Then we emit cb */
        for (int i = 0; i < numMessageIndices; i++) {
            T &outgoingMessage = outgoingMessages[s->messageIndices[i]];

            int flags = (i == numMessageIndices - 1) ? LAST : 0;

            /* Returning true will stop drainage short (such as when backpressure is too high) */
            if (cb(s, outgoingMessage, (IteratorFlags)(flags | (i == 0 ? FIRST : 0)))) {
                break;
            }
        }
    }

    void unlinkDrainableSubscriber(Subscriber *s) {
        if (s->prev) {
            s->prev->next = s->next;
        }
        if (s->next) {
            s->next->prev = s->prev;
        }
        /* If we are the head, then we also need to reset the head */
        if (drainableSubscribers == s) {
            drainableSubscribers = s->next;
        }
    }

    void destroySubscriber(Subscriber *s, bool alreadyUnlinked = false) {
        /* For all topics, unsubscribe */
        for (Topic *topicPtr : s->topics) {
            /* If we are the last subscriber, simply remove the whole topic */
            if (topicPtr->size() == 1) {
                topics.erase(topicPtr->name);
            } else {
                /* Otherwise just remove us */
                topicPtr->erase(s);
            }
        }

        /* We also need to unlink us */
        if (!alreadyUnlinked && s->needsDrainage()) {
            unlinkDrainableSubscriber(s);
        }

        delete s;
    }

    void freePendingSubscribers() {
        std::vector<Subscriber *> pending;
        pending.swap(pendingFree);

        for (Subscriber *s : pending) {
            destroySubscriber(s, true);
        }
    }

    bool wasFreed(Subscriber *s) {
        return s->pendingDestruction;
    }

    bool detached(Subscriber *s, Topic *t) {
        return wasFreed(s) || s->topics.find(t) == s->topics.end();
    }

    void deferOp(PendingOp &&op) {
        pendingOps.push_back(std::move(op));
        pending = true;
    }

    bool subscribePending(Topic *t, size_t from) {
        for (size_t i = from; i < pendingOps.size(); i++) {
            if (pendingOps[i].kind == PendingOp::SUBSCRIBE && pendingOps[i].topic == t) { return true; }
        }
        return false;
    }

    void eraseFromTopic(Subscriber *s, Topic *t, size_t opsFrom) {
        t->erase(s);
        if (t->empty() && !(pending && subscribePending(t, opsFrom))) {
            topics.erase(t->name);
        }
    }

    void leaveDraining() {
        if (--drainingDepth == 0 && pending && !applying) {
            applyPending();
        }
    }

    void applyPending() {
        applying = true;
        for (size_t i = 0; i < pendingOps.size(); i++) {
            PendingOp op = std::move(pendingOps[i]);
            switch (op.kind) {
            case PendingOp::SUBSCRIBE:   op.topic->insert(op.subscriber); break;
            case PendingOp::UNSUBSCRIBE: eraseFromTopic(op.subscriber, op.topic, i + 1); break;
            case PendingOp::PUBLISH:     publish(op.subscriber, op.topicName, std::move(op.message)); break;
            }
            pendingOps[i].topic = nullptr;
        }
        pendingOps.clear();
        freePendingSubscribers();
        pending = false;
        applying = false;
    }

public:

    TopicTree(std::function<bool(Subscriber *, T &, IteratorFlags)> cb) : cb(cb) {

    }

    bool isDraining() const { return drainingDepth > 0; }

    Topic *lookupTopic(std::string_view topic) {
        auto it = topics.find(topic);
        if (it == topics.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    int numSubscribers(Topic *t) {
        int count = (int) t->size();
        if (pending) {
            for (PendingOp &op : pendingOps) {
                if (op.topic == t) { count += op.kind == PendingOp::SUBSCRIBE ? 1 : -1; }
            }
        }
        return count;
    }

    Topic *subscribe(Subscriber *s, std::string_view topic) {
        if (isIterating(s)) { return nullptr; }
        Topic *topicPtr = lookupTopic(topic);
        if (!topicPtr) {
            Topic *newTopic = new Topic(topic);
            topics.insert({std::string_view(newTopic->name.data(), newTopic->name.length()), std::unique_ptr<Topic>(newTopic)});
            topicPtr = newTopic;
        }
        auto [it, inserted] = s->topics.insert(topicPtr);
        if (!inserted) { return nullptr; }
        if (topicPtr == iteratingTopic) {
            deferOp({PendingOp::SUBSCRIBE, s, topicPtr, {}, {}});
            return topicPtr;
        }
        topicPtr->insert(s);
        return topicPtr;
    }

    std::tuple<bool, bool, int> unsubscribe(Subscriber *s, std::string_view topic) {
        if (isIterating(s)) { return {false, false, -1}; }
        Topic *topicPtr = lookupTopic(topic);
        if (!topicPtr) { return {false, false, -1}; }
        if (s->topics.erase(topicPtr) == 0) { return {false, false, -1}; }
        if (topicPtr == iteratingTopic) {
            deferOp({PendingOp::UNSUBSCRIBE, s, topicPtr, {}, {}});
            return {true, s->topics.empty(), numSubscribers(topicPtr)};
        }
        int newCount = (int) topicPtr->size() - 1;
        eraseFromTopic(s, topicPtr, 0);
        return {true, s->topics.empty(), newCount};
    }

    Subscriber *createSubscriber() {
        return new Subscriber();
    }

    void freeSubscriber(Subscriber *s) {
        if (!s) { return; }
        if (drainingDepth > 0 || applying) {
            if (s->needsDrainage()) { unlinkDrainableSubscriber(s); s->numMessageIndices = 0; }
            pendingFree.push_back(s);
            s->pendingDestruction = true;
            pending = true;
            return;
        }
        destroySubscriber(s);
    }

    void drain(Subscriber *s) {
        if (s->needsDrainage()) {
            drainingDepth++;
            unlinkDrainableSubscriber(s);
            drainImpl(s);
            if (!drainableSubscribers) { outgoingMessages.clear(); }
            leaveDraining();
        }
    }

    void drain() {
        if (drainableSubscribers) {
            drainingDepth++;
            for (Subscriber *s = drainableSubscribers; s;) {
                Subscriber *next = s->next;
                if (!wasFreed(s)) { drainImpl(s); }
                s = next;
            }
            drainableSubscribers = nullptr;
            outgoingMessages.clear();
            leaveDraining();
        }
    }

    template <typename F>
    bool publishBig(Subscriber *sender, std::string_view topic, B &&bigMessage, F cb) {
        auto it = topics.find(topic);
        if (it == topics.end()) { return false; }
        if (drainingDepth > 0) {
            std::cerr << "Error: publishBig called while draining; route through publish()" << std::endl;
            return false;
        }
        Topic *topicPtr = it->second.get();
        drainingDepth++;
        iteratingTopic = topicPtr;
        for (Subscriber *s : *topicPtr) {
            if (sender != s) {
                if (pending && detached(s, topicPtr)) { continue; }
                cb(s, bigMessage);
            }
        }
        iteratingTopic = nullptr;
        leaveDraining();
        return true;
    }

    bool publish(Subscriber *sender, std::string_view topic, T &&message) {
        auto it = topics.find(topic);
        if (it == topics.end()) { return false; }
        Topic *topicPtr = it->second.get();
        if (drainingDepth > 0) {
            bool wanted = false;
            for (Subscriber *s : *topicPtr) {
                if (s != sender && !(pending && detached(s, topicPtr))) { wanted = true; break; }
            }
            deferOp({PendingOp::PUBLISH, sender, nullptr, std::string(topic), std::move(message)});
            return wanted;
        }
        if (outgoingMessages.size() >= UINT16_MAX) { drain(); }
        bool referencedMessage = false;
        drainingDepth++;
        iteratingTopic = topicPtr;
        for (Subscriber *s : *topicPtr) {
            if (sender != s) {
                if (pending && detached(s, topicPtr)) { continue; }
                if (s->numMessageIndices == 32) {
                    drain(s);
                    if (pending && detached(s, topicPtr)) { continue; }
                }
                referencedMessage = true;
                s->messageIndices[s->numMessageIndices++] = (uint16_t) outgoingMessages.size();
                if (s->numMessageIndices == 1) {
                    s->next = drainableSubscribers;
                    s->prev = nullptr;
                    if (s->next) { s->next->prev = s; }
                    drainableSubscribers = s;
                }
            }
        }
        iteratingTopic = nullptr;
        if (referencedMessage) { outgoingMessages.emplace_back(std::move(message)); }
        leaveDraining();
        return referencedMessage;
    }
};

}

#endif
