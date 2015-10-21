//
//  audio-playout.h
//  ndnrtc
//
//  Created by Peter Gusev on 4/14/14.
//  Copyright 2013-2015 Regents of the University of California
//

#ifndef __ndnrtc__audio_playout__
#define __ndnrtc__audio_playout__

#include "ndnrtc-common.h"
#include "playout.h"

namespace ndnrtc {
    namespace new_api {
        class AudioPlayout : public Playout
        {
        public:
            AudioPlayout(Consumer* consumer,
                         const boost::shared_ptr<statistics::StatisticsStorage>& statStorage);
            ~AudioPlayout();
            
        private:
            bool
            playbackPacket(int64_t packetTsLocal, PacketData* data,
                           PacketNumber playbackPacketNo,
                           PacketNumber sequencePacketNo,
                           PacketNumber pairedPacketNo,
                           bool isKey, double assembledLevel);
        };
    }
}

#endif /* defined(__ndnrtc__audio_playout__) */
