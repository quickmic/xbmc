/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "EpgTagsContainer.h"

#include "addons/kodi-dev-kit/include/kodi/c-api/addon-instance/pvr/pvr_epg.h"
#include "pvr/epg/EpgDatabase.h"
#include "pvr/epg/EpgInfoTag.h"
#include "pvr/epg/EpgTagsCache.h"
#include "utils/log.h"

#include <algorithm>
#include <ranges>

using namespace PVR;

namespace
{
const CDateTimeSpan ONE_SECOND(0, 0, 0, 1);
}

CPVREpgTagsContainer::CPVREpgTagsContainer(int iEpgID,
                                           const std::shared_ptr<CPVREpgChannelData>& channelData,
                                           const std::shared_ptr<CPVREpgDatabase>& database)
  : m_iEpgID(iEpgID),
    m_channelData(channelData),
    m_database(database),
    m_tagsCache(std::make_unique<CPVREpgTagsCache>(iEpgID, channelData, database, m_changedTags))
{
}

CPVREpgTagsContainer::~CPVREpgTagsContainer() = default;

void CPVREpgTagsContainer::SetEpgID(int iEpgID)
{
  m_iEpgID = iEpgID;
  for (const auto& [_, tag] : m_changedTags)
    tag->SetEpgID(iEpgID);
}

void CPVREpgTagsContainer::SetChannelData(const std::shared_ptr<CPVREpgChannelData>& data)
{
  m_channelData = data;
  m_tagsCache->SetChannelData(data);
  for (const auto& [_, tag] : m_changedTags)
    tag->SetChannelData(data);
}

namespace
{

void ResolveConflictingTags(const std::shared_ptr<CPVREpgInfoTag>& changedTag,
                            std::vector<std::shared_ptr<CPVREpgInfoTag>>& tags)
{
  const CDateTime changedTagStart = changedTag->StartAsUTC();
  const CDateTime changedTagEnd = changedTag->EndAsUTC();

  for (auto it = tags.begin(); it != tags.end();)
  {
    bool bInsert = false;

    if (changedTagEnd > (*it)->StartAsUTC() && changedTagStart < (*it)->EndAsUTC())
    {
      it = tags.erase(it);

      if (it == tags.end())
      {
        bInsert = true;
      }
    }
    else if ((*it)->StartAsUTC() >= changedTagEnd)
    {
      bInsert = true;
    }
    else
    {
      ++it;
    }

    if (bInsert)
    {
      tags.emplace(it, changedTag);
      break;
    }
  }
}

bool FixOverlap(const std::shared_ptr<CPVREpgInfoTag>& previousTag,
                const std::shared_ptr<CPVREpgInfoTag>& currentTag)
{
  if (!previousTag)
    return true;

  if (previousTag->EndAsUTC() >= currentTag->EndAsUTC())
  {
    // delete the current tag. it's completely overlapped
    CLog::LogF(LOGDEBUG,
               "Erasing completely overlapped event from EPG timeline "
               "({} - {} - {} - {}) "
               "({} - {} - {} - {}).",
               previousTag->UniqueBroadcastID(), previousTag->Title(),
               previousTag->StartAsUTC().GetAsDBDateTime(),
               previousTag->EndAsUTC().GetAsDBDateTime(), currentTag->UniqueBroadcastID(),
               currentTag->Title(), currentTag->StartAsUTC().GetAsDBDateTime(),
               currentTag->EndAsUTC().GetAsDBDateTime());

    return false;
  }
  else if (previousTag->EndAsUTC() > currentTag->StartAsUTC())
  {
    // fix the end time of the predecessor of the event
    CLog::LogF(LOGDEBUG,
               "Fixing partly overlapped event in EPG timeline "
               "({} - {} - {} - {}) "
               "({} - {} - {} - {}).",
               previousTag->UniqueBroadcastID(), previousTag->Title(),
               previousTag->StartAsUTC().GetAsDBDateTime(),
               previousTag->EndAsUTC().GetAsDBDateTime(), currentTag->UniqueBroadcastID(),
               currentTag->Title(), currentTag->StartAsUTC().GetAsDBDateTime(),
               currentTag->EndAsUTC().GetAsDBDateTime());

    previousTag->SetEndFromUTC(currentTag->StartAsUTC());
  }
  return true;
}

} // unnamed namespace

bool CPVREpgTagsContainer::UpdateEntries(const CPVREpgTagsContainer& tags)
{
  if (tags.m_changedTags.empty())
    return false;

  if (m_database)
  {
    const CDateTime minEventEnd = (*tags.m_changedTags.cbegin()).second->StartAsUTC() + ONE_SECOND;
    const CDateTime maxEventStart = (*tags.m_changedTags.crbegin()).second->EndAsUTC();

    std::vector<std::shared_ptr<CPVREpgInfoTag>> existingTags =
        m_database->GetEpgTagsByMinEndMaxStartTime(m_iEpgID, minEventEnd, maxEventStart);

    if (!m_changedTags.empty())
    {
      // Fix data inconsistencies
      for (const auto& [_, tag] : m_changedTags)
      {
        if (tag->EndAsUTC() > minEventEnd && tag->StartAsUTC() < maxEventStart)
        {
          // tag is in queried range, thus it could cause inconsistencies...
          ResolveConflictingTags(tag, existingTags);
        }
      }
    }

    bool bResetCache = false;
    for (const auto& [_, tag] : tags.m_changedTags)
    {
      tag->SetChannelData(m_channelData);
      tag->SetEpgID(m_iEpgID);

      const auto it = std::ranges::find_if(existingTags, [&tag](const auto& t)
                                           { return t->StartAsUTC() == tag->StartAsUTC(); });

      if (it != existingTags.cend())
      {
        const std::shared_ptr<CPVREpgInfoTag>& existingTag = *it;

        existingTag->SetChannelData(m_channelData);
        existingTag->SetEpgID(m_iEpgID);

        if (existingTag->Update(*tag, false))
        {
          // tag differs from existing tag and must be persisted
          m_changedTags.insert({existingTag->StartAsUTC(), existingTag});
          bResetCache = true;
        }
      }
      else
      {
        // new tags must always be persisted
        m_changedTags.insert({tag->StartAsUTC(), tag});
        bResetCache = true;
      }
    }

    if (bResetCache)
      m_tagsCache->Reset();
  }
  else
  {
    for (const auto& [_, tag] : tags.m_changedTags)
      UpdateEntry(tag);
  }

  return true;
}

void CPVREpgTagsContainer::FixOverlappingEvents(
    std::vector<std::shared_ptr<CPVREpgInfoTag>>& tags) const
{
  bool bResetCache = false;

  std::shared_ptr<CPVREpgInfoTag> previousTag;
  for (auto it = tags.begin(); it != tags.end();)
  {
    const std::shared_ptr<CPVREpgInfoTag> currentTag = *it;
    if (FixOverlap(previousTag, currentTag))
    {
      previousTag = currentTag;
      ++it;
    }
    else
    {
      it = tags.erase(it);
      bResetCache = true;
    }
  }

  if (bResetCache)
    m_tagsCache->Reset();
}

void CPVREpgTagsContainer::FixOverlappingEvents(
    std::map<CDateTime, std::shared_ptr<CPVREpgInfoTag>>& tags) const
{
  std::shared_ptr<CPVREpgInfoTag> previousTag;
  if (std::erase_if(tags,
                    [&previousTag](const auto& entry)
                    {
                      const auto& [_, currentTag] = entry;
                      if (FixOverlap(previousTag, currentTag))
                      {
                        previousTag = currentTag;
                        return false;
                      }
                      return true;
                    }) > 0)
  {
    m_tagsCache->Reset();
  }
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::CreateEntry(
    const std::shared_ptr<CPVREpgInfoTag>& tag) const
{
  if (tag)
  {
    tag->SetChannelData(m_channelData);
  }
  return tag;
}

std::vector<std::shared_ptr<CPVREpgInfoTag>> CPVREpgTagsContainer::CreateEntries(
    const std::vector<std::shared_ptr<CPVREpgInfoTag>>& tags) const
{
  for (auto& tag : tags)
  {
    tag->SetChannelData(m_channelData);
  }
  return tags;
}

bool CPVREpgTagsContainer::UpdateEntry(const std::shared_ptr<CPVREpgInfoTag>& tag)
{
  tag->SetChannelData(m_channelData);
  tag->SetEpgID(m_iEpgID);

  std::shared_ptr<CPVREpgInfoTag> existingTag = GetTag(tag->StartAsUTC());
  if (existingTag)
  {
    if (existingTag->Update(*tag, false))
    {
      // tag differs from existing tag and must be persisted
      m_changedTags.try_emplace(existingTag->StartAsUTC(), existingTag);
      m_tagsCache->Reset();
    }
  }
  else
  {
    // new tags must always be persisted
    m_changedTags.try_emplace(tag->StartAsUTC(), tag);
    m_tagsCache->Reset();
  }

  return true;
}

bool CPVREpgTagsContainer::DeleteEntry(const std::shared_ptr<CPVREpgInfoTag>& tag)
{
  m_changedTags.erase(tag->StartAsUTC());
  m_deletedTags.try_emplace(tag->StartAsUTC(), tag);
  m_tagsCache->Reset();
  return true;
}

void CPVREpgTagsContainer::Cleanup(const CDateTime& time)
{
  if (std::erase_if(m_changedTags,
                    [this, &time](const auto& entry)
                    {
                      const auto& [tm, currentTag] = entry;
                      if (currentTag->EndAsUTC() >= time)
                        return false;

                      const auto it = m_deletedTags.find(tm);
                      if (it != m_deletedTags.cend())
                        m_deletedTags.erase(it);

                      return true;
                    }) > 0)
  {
    m_tagsCache->Reset();
  }

  if (m_database)
    m_database->DeleteEpgTags(m_iEpgID, time);
}

void CPVREpgTagsContainer::Clear()
{
  m_changedTags.clear();
  m_tagsCache->Reset();
}

bool CPVREpgTagsContainer::IsEmpty() const
{
  if (!m_changedTags.empty())
    return false;

  if (m_database)
    return !m_database->HasTags(m_iEpgID);

  return true;
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::GetTag(const CDateTime& startTime) const
{
  const auto it = m_changedTags.find(startTime);
  if (it != m_changedTags.cend())
    return (*it).second;

  if (m_database)
    return CreateEntry(m_database->GetEpgTagByStartTime(m_iEpgID, startTime));

  return {};
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::GetTag(unsigned int iUniqueBroadcastID) const
{
  if (iUniqueBroadcastID == EPG_TAG_INVALID_UID)
    return {};

  const auto it =
      std::ranges::find_if(m_changedTags, [iUniqueBroadcastID](const auto& tag)
                           { return tag.second->UniqueBroadcastID() == iUniqueBroadcastID; });

  if (it != m_changedTags.cend())
    return (*it).second;

  if (m_database)
    return CreateEntry(m_database->GetEpgTagByUniqueBroadcastID(m_iEpgID, iUniqueBroadcastID));

  return {};
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::GetTagByDatabaseID(int iDatabaseID) const
{
  if (iDatabaseID <= 0)
    return {};

  const auto it = std::ranges::find_if(m_changedTags, [iDatabaseID](const auto& tag)
                                       { return tag.second->DatabaseID() == iDatabaseID; });

  if (it != m_changedTags.cend())
    return (*it).second;

  if (m_database)
    return CreateEntry(m_database->GetEpgTagByDatabaseID(m_iEpgID, iDatabaseID));

  return {};
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::GetTagBetween(const CDateTime& start,
                                                                    const CDateTime& end) const
{
  for (const auto& [_, tag] : m_changedTags)
  {
    if (tag->StartAsUTC() >= start)
    {
      if (tag->EndAsUTC() <= end)
        return tag;
      else
        break;
    }
  }

  if (m_database)
  {
    const std::vector<std::shared_ptr<CPVREpgInfoTag>> tags =
        CreateEntries(m_database->GetEpgTagsByMinStartMaxEndTime(m_iEpgID, start, end));
    if (!tags.empty())
    {
      if (tags.size() > 1)
        CLog::LogF(LOGWARNING, "Got multiple tags. Picking up the first.");

      return tags.front();
    }
  }

  return {};
}

bool CPVREpgTagsContainer::UpdateActiveTag()
{
  return m_tagsCache->Refresh();
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::GetActiveTag() const
{
  return m_tagsCache->GetNowActiveTag();
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::GetLastEndedTag() const
{
  return m_tagsCache->GetLastEndedTag();
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::GetNextStartingTag() const
{
  return m_tagsCache->GetNextStartingTag();
}

std::shared_ptr<CPVREpgInfoTag> CPVREpgTagsContainer::CreateGapTag(const CDateTime& start,
                                                                   const CDateTime& end) const
{
  return std::make_shared<CPVREpgInfoTag>(m_channelData, m_iEpgID, start, end, true);
}

void CPVREpgTagsContainer::MergeTags(const CDateTime& minEventEnd,
                                     const CDateTime& maxEventStart,
                                     std::vector<std::shared_ptr<CPVREpgInfoTag>>& tags) const
{
  for (const auto& [_, tag] : m_changedTags)
  {
    if (tag->EndAsUTC() > minEventEnd && tag->StartAsUTC() < maxEventStart)
      tags.emplace_back(tag);
  }

  if (!tags.empty())
    FixOverlappingEvents(tags);
}

std::vector<std::shared_ptr<CPVREpgInfoTag>> CPVREpgTagsContainer::GetTimeline(
    const CDateTime& timelineStart,
    const CDateTime& timelineEnd,
    const CDateTime& minEventEnd,
    const CDateTime& maxEventStart) const
{
  if (m_database)
  {
    std::vector<std::shared_ptr<CPVREpgInfoTag>> tags;

    bool loadFromDb = true;
    if (!m_changedTags.empty())
    {
      const CDateTime lastEnd = m_database->GetLastEndTime(m_iEpgID);
      if (!lastEnd.IsValid() || lastEnd < minEventEnd)
      {
        // nothing in the db yet. take what we have in memory.
        loadFromDb = false;
        MergeTags(minEventEnd, maxEventStart, tags);
      }
    }

    if (loadFromDb)
    {
      tags = m_database->GetEpgTagsByMinEndMaxStartTime(m_iEpgID, minEventEnd, maxEventStart);

      if (!m_changedTags.empty())
      {
        // Fix data inconsistencies
        for (const auto& [_, tag] : m_changedTags)
        {
          if (tag->EndAsUTC() > minEventEnd && tag->StartAsUTC() < maxEventStart)
          {
            // tag is in queried range, thus it could cause inconsistencies...
            ResolveConflictingTags(tag, tags);
          }
        }

        // Append missing tags
        MergeTags(tags.empty() ? minEventEnd : tags.back()->EndAsUTC(), maxEventStart, tags);
      }
    }

    tags = CreateEntries(tags);

    std::vector<std::shared_ptr<CPVREpgInfoTag>> result;

    for (const auto& epgTag : tags)
    {
      if (!result.empty())
      {
        const CDateTime currStart = epgTag->StartAsUTC();
        const CDateTime prevEnd = result.back()->EndAsUTC();
        if ((currStart - prevEnd) >= ONE_SECOND)
        {
          // insert gap tag before current tag
          result.emplace_back(CreateGapTag(prevEnd, currStart));
        }
      }

      result.emplace_back(epgTag);
    }

    if (result.empty())
    {
      // create single gap tag
      CDateTime maxEnd = m_database->GetMaxEndTime(m_iEpgID, minEventEnd);
      if (!maxEnd.IsValid() || maxEnd < timelineStart)
        maxEnd = timelineStart;

      CDateTime minStart = m_database->GetMinStartTime(m_iEpgID, maxEventStart);
      if (!minStart.IsValid() || minStart > timelineEnd)
        minStart = timelineEnd;

      result.emplace_back(CreateGapTag(maxEnd, minStart));
    }
    else
    {
      if (result.front()->StartAsUTC() > minEventEnd)
      {
        // prepend gap tag
        CDateTime maxEnd = m_database->GetMaxEndTime(m_iEpgID, minEventEnd);
        if (!maxEnd.IsValid() || maxEnd < timelineStart)
          maxEnd = timelineStart;

        result.insert(result.begin(), CreateGapTag(maxEnd, result.front()->StartAsUTC()));
      }

      if (result.back()->EndAsUTC() < maxEventStart)
      {
        // append gap tag
        CDateTime minStart = m_database->GetMinStartTime(m_iEpgID, maxEventStart);
        if (!minStart.IsValid() || minStart > timelineEnd)
          minStart = timelineEnd;

        result.emplace_back(CreateGapTag(result.back()->EndAsUTC(), minStart));
      }
    }

    return result;
  }

  return {};
}

std::vector<std::shared_ptr<CPVREpgInfoTag>> CPVREpgTagsContainer::GetAllTags() const
{
  if (m_database)
  {
    std::vector<std::shared_ptr<CPVREpgInfoTag>> tags;
    if (!m_changedTags.empty() && !m_database->HasTags(m_iEpgID))
    {
      // nothing in the db yet. take what we have in memory.
      std::ranges::copy(std::views::values(m_changedTags), std::back_inserter(tags));

      FixOverlappingEvents(tags);
    }
    else
    {
      tags = m_database->GetAllEpgTags(m_iEpgID);

      if (!m_changedTags.empty())
      {
        // Fix data inconsistencies
        for (const auto& [_, tag] : m_changedTags)
        {
          ResolveConflictingTags(tag, tags);
        }
      }
    }

    return CreateEntries(tags);
  }

  return {};
}

std::pair<CDateTime, CDateTime> CPVREpgTagsContainer::GetFirstAndLastUncommittedEPGDate() const
{
  if (m_changedTags.empty())
    return {};

  return {(*m_changedTags.cbegin()).second->StartAsUTC(),
          (*m_changedTags.crbegin()).second->EndAsUTC()};
}

bool CPVREpgTagsContainer::NeedsSave() const
{
  return !m_changedTags.empty() || !m_deletedTags.empty();
}

void CPVREpgTagsContainer::QueuePersistQuery()
{
  if (m_database)
  {
    m_database->Lock();

    CLog::LogFC(LOGDEBUG, LOGEPG, "EPG Tags Container: Updating {}, deleting {} events...",
                m_changedTags.size(), m_deletedTags.size());

    for (const auto& [_, tag] : m_deletedTags)
      m_database->QueueDeleteTagQuery(*tag);

    m_deletedTags.clear();

    FixOverlappingEvents(m_changedTags);

    for (const auto& [_, tag] : m_changedTags)
    {
      // remove any conflicting events from database before persisting the new event
      m_database->QueueDeleteEpgTagsByMinEndMaxStartTimeQuery(
          m_iEpgID, tag->StartAsUTC() + ONE_SECOND, tag->EndAsUTC() - ONE_SECOND);

      tag->QueuePersistQuery(m_database);
    }

    Clear();

    m_database->Unlock();
  }
}

void CPVREpgTagsContainer::QueueDelete()
{
  if (m_database)
    m_database->QueueDeleteEpgTags(m_iEpgID);

  Clear();
}
