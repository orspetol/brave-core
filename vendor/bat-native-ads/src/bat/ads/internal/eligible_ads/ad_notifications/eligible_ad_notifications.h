/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BAT_ADS_INTERNAL_ELIGIBLE_ADS_ELIGIBLE_AD_NOTIFICATIONS_H_
#define BAT_ADS_INTERNAL_ELIGIBLE_ADS_ELIGIBLE_AD_NOTIFICATIONS_H_

#include "bat/ads/internal/ad_events/ad_event_info.h"
#include "bat/ads/internal/bundle/creative_ad_notification_info.h"

namespace ads {

namespace ad_targeting {
namespace geographic {
class SubdivisionTargeting;
}  // namespace geographic
}  // namespace ad_targeting

namespace ad_notifications {

class EligibleAds {
 public:
  EligibleAds(
      ad_targeting::geographic::SubdivisionTargeting* subdivision_targeting);

  ~EligibleAds();

  CreativeAdNotificationList Get(
      const CreativeAdNotificationList& ads,
      const CreativeAdInfo& last_delivered_ad,
      const AdEventList& ad_events);

 private:
  ad_targeting::geographic::SubdivisionTargeting* subdivision_targeting_;

  CreativeAdNotificationList RemoveSeenAdvertisersAndRoundRobinIfNeeded(
      const CreativeAdNotificationList& ads) const;

  CreativeAdNotificationList RemoveSeenAdsAndRoundRobinIfNeeded(
      const CreativeAdNotificationList& ads) const;

  CreativeAdNotificationList FrequencyCap(
      const CreativeAdNotificationList& ads,
      const CreativeAdInfo& last_delivered_ad,
      const AdEventList& ad_events) const;
};

}  // namespace ad_notifications
}  // namespace ads

#endif  // BAT_ADS_INTERNAL_ELIGIBLE_ADS_ELIGIBLE_AD_NOTIFICATIONS_H_
