/* Client-side IP geolocation using ip-api.com (free tier, no API key) */

interface GeoResult {
  country: string;
  countryCode: string;
  city: string;
  latitude: number;
  longitude: number;
}

const cache = new Map<string, GeoResult>();

export async function getGeo(ip: string): Promise<GeoResult | null> {
  if (!ip || ip === "0.0.0.0" || ip === "127.0.0.1" || ip === "::1") return null;

  const cached = cache.get(ip);
  if (cached) return cached;

  try {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 5000);
    const res = await fetch(`https://ip-api.com/json/${ip}`, { signal: controller.signal });
    clearTimeout(timeout);
    if (!res.ok) return null;
    const data = await res.json();
    if (data.status !== "success") return null;

    const result: GeoResult = {
      country: data.country,
      countryCode: data.countryCode,
      city: data.city,
      latitude: data.lat,
      longitude: data.lon,
    };

    cache.set(ip, result);
    return result;
  } catch {
    return null;
  }
}
