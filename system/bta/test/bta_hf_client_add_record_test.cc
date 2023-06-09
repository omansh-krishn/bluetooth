/******************************************************************************
 *
 *  Copyright 2021 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#include <base/logging.h>
#include <gtest/gtest.h>

#include "bta/hf_client/bta_hf_client_sdp.cc"
#include "bta/include/bta_hf_client_api.h"
#include "btif/src/btif_hf_client.cc"
#include "types/bluetooth/uuid.h"

static uint16_t gVersion;

bool SDP_AddProtocolList(uint32_t handle, uint16_t num_elem,
                         tSDP_PROTOCOL_ELEM* p_elem_list) {
  return false;
}
bool SDP_AddServiceClassIdList(uint32_t handle, uint16_t num_services,
                               uint16_t* p_service_uuids) {
  return false;
}
bool SDP_AddProfileDescriptorList(uint32_t handle, uint16_t profile_uuid,
                                  uint16_t version) {
  gVersion = version;
  return false;
}
bool SDP_AddAttribute(uint32_t handle, uint16_t attr_id, uint8_t attr_type,
                      uint32_t attr_len, uint8_t* p_val) {
  return false;
}
bool SDP_AddUuidSequence(uint32_t handle, uint16_t attr_id, uint16_t num_uuids,
                         uint16_t* p_uuids) {
  return false;
}

class BtaHfClientAddRecordTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gVersion = 0;
  }

  void TearDown() override {}
};

TEST_F(BtaHfClientAddRecordTest, test_hf_client_add_record) {
  tBTA_HF_CLIENT_FEAT features = get_default_hf_client_features();
  uint32_t sdp_handle = 0;
  uint8_t scn = 0;

  bta_hf_client_add_record("Handsfree", scn, features, sdp_handle);
  ASSERT_EQ(gVersion, get_default_hfp_version());
}

