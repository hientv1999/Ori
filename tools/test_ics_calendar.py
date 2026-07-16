#!/usr/bin/env python3
"""
test_ics_calendar.py — quick test script for a published/shared Outlook
calendar ICS feed (Exchange's "publish calendar" mechanism).

Fetches the feed and expands recurring events (RRULE/EXDATE) into concrete
occurrences before picking the next 5. Recurrence expansion matters here: a
single VEVENT block with an RRULE represents a whole series (e.g. "every
month"), not one meeting — naively taking the first 5 VEVENT blocks in file
order would mix up series with actual occurrences instead of giving you the
next 5 real meetings.

Requirements:
    pip install requests icalendar recurring-ical-events

Usage:
    python tools/test_ics_calendar.py "<ics url>"
"""

import sys
from datetime import date, datetime, timedelta, timezone

import icalendar
import recurring_ical_events
import requests


def to_sortable(dt):
    """Normalize a DTSTART/DTEND value (datetime.date for all-day events,
    datetime.datetime otherwise) into a timezone-aware datetime so mixed
    all-day/timed events can be sorted against each other."""
    if isinstance(dt, datetime):
        return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)
    if isinstance(dt, date):
        return datetime(dt.year, dt.month, dt.day, tzinfo=timezone.utc)
    raise TypeError(f"Unexpected DTSTART/DTEND type: {type(dt)}")


def format_when(dt):
    if isinstance(dt, datetime):
        return dt.strftime("%Y-%m-%d %H:%M %z")
    return f"{dt.isoformat()} (all-day)"


def main():
    if len(sys.argv) < 2:
        print("Usage: python test_ics_calendar.py <ics-url>")
        sys.exit(1)

    url = sys.argv[1]
    print(f"Fetching {url} ...")
    resp = requests.get(url, timeout=20)
    resp.raise_for_status()
    print(f"Got {len(resp.content)} bytes.\n")

    calendar = icalendar.Calendar.from_ical(resp.content)

    now = datetime.now(timezone.utc)
    window_end = now + timedelta(days=180)  # look ~6 months ahead to be sure we find 5

    occurrences = recurring_ical_events.of(calendar).between(now, window_end)
    occurrences.sort(key=lambda e: to_sortable(e["DTSTART"].dt))

    n = min(5, len(occurrences))
    print(f"Next {n} upcoming meeting(s) (of {len(occurrences)} found in the next 180 days):\n")

    for e in occurrences[:5]:
        start = e["DTSTART"].dt
        end = e["DTEND"].dt if "DTEND" in e else None
        title = str(e.get("SUMMARY", "(no title)"))
        location = str(e.get("LOCATION") or "").replace("\\,", ",").replace("\\;", ";")
        organizer = e.get("ORGANIZER")

        print(f"- {format_when(start)}  ->  {format_when(end) if end else '?'}")
        print(f"  {title}")
        if location:
            print(f"  @ {location}")
        if organizer:
            print(f"  organizer: {organizer}")
        else:
            print("  organizer: (not present in this feed)")
        print()


if __name__ == "__main__":
    main()
