#!/usr/bin/env python3
"""
parse_sharing_metadata.py — pulls the useful fields out of a sharing_metadata.xml
attachment (the payload Outlook embeds in a calendar-sharing invitation email,
schema http://schemas.microsoft.com/sharing/2008).

Extracts:
    - who shared it: Initiator/Name + Initiator/SmtpAddress
    - who they shared it with: Provider/@TargetRecipients
    - the fetchable feed URLs: Provider/BrowseUrl (HTML view) + Provider/ICalUrl (ICS feed)

Standard library only (xml.etree.ElementTree) — no dependencies to install.

Usage:
    python tools/parse_sharing_metadata.py sharing_metadata.xml
"""

import sys
import xml.etree.ElementTree as ET

NS_SHARING = "http://schemas.microsoft.com/sharing/2008"
NS_EXCHANGE = "http://schemas.microsoft.com/exchange/sharing/2008"


def main():
    if len(sys.argv) < 2:
        print("Usage: python parse_sharing_metadata.py <sharing_metadata.xml>")
        sys.exit(1)

    tree = ET.parse(sys.argv[1])
    root = tree.getroot()

    def find(path):
        el = root.find(path, {"s": NS_SHARING, "e": NS_EXCHANGE})
        return el.text if el is not None else None

    data_type = find("s:DataType")
    name = find("s:Initiator/s:Name")
    smtp = find("s:Initiator/s:SmtpAddress")

    provider = root.find("s:Invitation/s:Providers/s:Provider", {"s": NS_SHARING})
    target = provider.get("TargetRecipients") if provider is not None else None
    provider_type = provider.get("Type") if provider is not None else None
    browse_url = find("s:Invitation/s:Providers/s:Provider/e:BrowseUrl")
    ical_url = find("s:Invitation/s:Providers/s:Provider/e:ICalUrl")

    print(f"Data type:        {data_type}")
    print(f"Shared by:        {name} <{smtp}>")
    print(f"Shared with:      {target}")
    print(f"Sharing mechanism: {provider_type}")
    print(f"Browse (HTML) URL: {browse_url}")
    print(f"ICS feed URL:      {ical_url}")


if __name__ == "__main__":
    main()
