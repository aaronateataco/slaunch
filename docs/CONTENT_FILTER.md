# Adult Content Filtering

## Overview

The slaunch menu now supports filtering of adult/mature content when fetching news articles and cover art from online sources. This helps ensure that inappropriate content is not downloaded and displayed, especially on shared systems or for younger users.

## Features

The content filter checks multiple sources for adult content:

### News Fetcher
- **Nintendo News**: Analyzes article titles, summaries, and categories from nintendo.com/us/whatsnew
- **Steam News**: Filters news posts for games based on title and content analysis

### Cover/Art Fetcher
- **SteamGridDB**: Checks game search results and grid metadata
- **Steam Store**: Scans store API responses for adult content tags and metadata

## Content Filtering Criteria

The filter identifies adult content based on:

1. **Explicit Keywords**: Searches for explicit sexual, violent, or adult-related terminology
2. **Game Ratings**:
   - ESRB "Adults Only (AO)" ratings
   - PEGI 18 ratings
   - USK 18 ratings
3. **Content Tags**: Filters based on Steam store content tags like:
   - Sexual Content
   - Nudity
   - Explicit Sexual Content
   - Mature Content markers
4. **Known Adult Titles**: Database of known adult games (expandable)

## Configuration

### Enabling/Disabling the Filter

The filter is controlled via a configuration file:

**Location**: `sdmc:/slaunch/config/content_filter.txt`

**Format**:
```
enabled=1
```

- `enabled=1` - Content filter is active
- `enabled=0` - Content filter is disabled (default)

### Configuration File Creation

The configuration file is automatically created when you first enable the filter. You can also manually create it or modify it:

```bash
# Enable the filter
echo "enabled=1" > sdmc:/slaunch/config/content_filter.txt

# Disable the filter
echo "enabled=0" > sdmc:/slaunch/config/content_filter.txt
```

## Behavior

### When Enabled

When the content filter is enabled:

1. **News Articles**: Articles identified as adult content are skipped and not displayed
2. **Cover Art**: 
   - Games flagged as adult are not searched on SteamGridDB
   - Cover images from adult games are not downloaded
   - A "Filtered" status is logged instead of attempting the fetch
3. **Screenshots**: Steam screenshots for flagged games are skipped

### Logging

All fetch operations are logged to `sdmc:/slaunch/covers.log` with status information:

```
filtered http=0 curl=0 bytes=0 name=Adult Game Name
grid-filtered http=200 curl=0 bytes=1024 name=Game Title
steam-filtered http=200 curl=0 bytes=2048 name=Game Title
```

### Visual Feedback

- Games filtered due to adult content will show a blank cover (no image)
- No error is displayed; filtering happens silently in the background
- Status is logged for debugging purposes

## API Integration

### News Filtering

```cpp
// Returns true if the news article should be filtered
bool ShouldFilterNewsArticle(const std::string &title,
                             const std::string &summary,
                             const std::string &category);

// Returns true if Steam news should be filtered
bool ShouldFilterSteamNews(const std::string &title,
                           const std::string &content);
```

### Cover Art Filtering

```cpp
// Returns true if the game name indicates adult content
bool ShouldFilterGameByName(const std::string &title);

// Returns true if art metadata contains adult content indicators
bool ShouldFilterArtMetadata(const std::string &json_body,
                             const std::string &source);

// Returns true if Steam tags indicate adult content
bool ShouldFilterBySteamTags(const std::string &json_body);
```

### Rating-Based Filtering

```cpp
// Returns true if the ESRB rating indicates adult content
bool ShouldFilterByEsrb(const std::string &esrb_rating);

// Returns true if PEGI rating indicates adult content
bool ShouldFilterByPegi(const std::string &pegi_rating);

// Returns true if USK rating indicates adult content
bool ShouldFilterByUsk(const std::string &usk_rating);
```

## Implementation Details

### Source Code

- **Filter Logic**: `projects/sMenu/source/sl/menu/net/ContentFilter.cpp`
- **Filter Header**: `projects/sMenu/include/sl/menu/net/ContentFilter.hpp`
- **News Integration**: `projects/sMenu/source/sl/menu/news/News.cpp`
- **Cover Integration**: `projects/sMenu/source/sl/menu/ui/Menu_Flow.cpp`

### Build System

The content filter is automatically included in the build:

- Source files are in `source/sl/menu/net`
- Include files are in `include/sl/menu/net`
- No additional Makefile configuration needed

### State Tracking

A new `CoverState::Filtered` enum value has been added to track when covers are filtered:

```cpp
enum class CoverState { Idle, NoKey, BadKey, Searching, NoMatch, NoArt, Failed, Filtered, Got };
```

## Future Enhancements

Potential improvements:

1. **Expanded Adult Title Database**: Maintain a more comprehensive list of known adult games
2. **Customizable Filtering Levels**:
   - Strict: Filters PEGI 16, mature content
   - Moderate: Filters PEGI 18, adult only
   - Permissive: Only filters explicit sexual content
3. **API Integration**: Direct queries to game rating databases (IGDB, RAWG, etc.)
4. **UI Settings**: In-menu configuration instead of file-based config
5. **Multi-Language Support**: Content keyword filtering in different languages

## Troubleshooting

### Filter Not Working

1. Check that the config file exists: `sdmc:/slaunch/config/content_filter.txt`
2. Verify it contains `enabled=1` (case-sensitive)
3. Restart the menu for changes to take effect

### Missing Content

If content you expect is being filtered:

1. Check the log file: `sdmc:/slaunch/covers.log`
2. Look for "filtered" entries with the game name
3. The filter may be too aggressive for your needs - adjust settings if needed

### Enabling Debug Output

Look at the covers.log file for detailed information:

```bash
tail -f sdmc:/slaunch/covers.log | grep filtered
```

## Technical Notes

- Filtering is performed **before** any network requests or image downloads
- Failed filtering does not prevent the menu from operating
- Content is cached locally, so filtering changes only apply to new fetches
- The filter uses string matching and keyword detection (case-insensitive)
