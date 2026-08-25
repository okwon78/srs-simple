// srs_simple — 원본: trunk/src/protocol/srs_protocol_rtmp_msg_array.hpp
// play 송신 루프가 consumer 큐에서 메시지를 배치로 꺼낼 때 쓰는 포인터 배열.
#ifndef SRS_PROTOCOL_MESSAGE_ARRAY_HPP
#define SRS_PROTOCOL_MESSAGE_ARRAY_HPP

#include <srs_core.hpp>

class SrsSharedPtrMessage;

// The class to auto free the shared ptr message array.
// When you need to get some messages, for instance, from the consumer queue,
// create a message array, whose msgs can be used to accept the msgs,
// then send each message and set to NULL.
//
// @remark: the user must free all msgs in the array, for the protocol stack
//       provides an api to send messages, @see send_and_free_messages
class SrsMessageArray
{
public:
    // When the user has already sent all msgs, please set them to NULL,
    // for instance, msg= msgs.msgs[i], msgs.msgs[i]=NULL, send(msg),
    // where send(msg) will always send and free it.
    SrsSharedPtrMessage** msgs;
    int max;
public:
    // Create the msg array, initialize the array to NULL ptrs.
    SrsMessageArray(int max_msgs);
    // Free the msgs not sent out(not NULL).
    virtual ~SrsMessageArray();
public:
    // Free the specified count of messages.
    virtual void free(int count);
private:
    // Zero initialize the message array.
    virtual void zero(int count);
};

#endif
