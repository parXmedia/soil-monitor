// Alert delivery.
//
// The system recorded that soil went dry and that the sensor stopped reporting,
// but never told anyone. For a garden monitor the notification is the product;
// the dashboard is the audit trail.
//
// public.evaluate_alerts() latches state in the database and returns only
// transitions, so this function delivers each raise/clear exactly once and can
// be polled as often as you like without spamming.
//
// Invoke on a schedule (pg_cron + pg_net, or any external scheduler) with the
// shared cron secret in the Authorization header.

import { constantTimeEqual } from "../soil-api/soil_api.ts";

interface AlertTransition {
  device_id: string;
  alert_kind: "dry_soil" | "sensor_offline" | "battery_low";
  transition: "raised" | "cleared";
  detail: Record<string, unknown> | null;
}

interface RuntimeConfig {
  supabaseUrl: string;
  serviceRoleKey: string;
  cronSecret: string;
  resendApiKey: string | null;
  alertFrom: string | null;
  alertTo: string | null;
  webhookUrl: string | null;
  dashboardUrl: string | null;
}

const encoder = new TextEncoder();

function requiredEnvironment(name: string): string {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`Missing required environment variable: ${name}`);
  return value;
}

function optionalEnvironment(name: string): string | null {
  const value = Deno.env.get(name)?.trim();
  return value ? value : null;
}

function loadConfig(): RuntimeConfig {
  const cronSecret = requiredEnvironment("SOIL_CRON_SECRET");
  if (cronSecret.length < 32) {
    throw new Error("SOIL_CRON_SECRET must be at least 32 characters");
  }

  const webhookUrl = optionalEnvironment("SOIL_ALERT_WEBHOOK_URL");
  if (webhookUrl !== null && !webhookUrl.startsWith("https://")) {
    throw new Error("SOIL_ALERT_WEBHOOK_URL must be HTTPS");
  }

  return {
    supabaseUrl: requiredEnvironment("SUPABASE_URL").replace(/\/$/, ""),
    serviceRoleKey: requiredEnvironment("SUPABASE_SERVICE_ROLE_KEY"),
    cronSecret,
    resendApiKey: optionalEnvironment("RESEND_API_KEY"),
    alertFrom: optionalEnvironment("SOIL_ALERT_FROM"),
    alertTo: optionalEnvironment("SOIL_ALERT_TO"),
    webhookUrl,
    dashboardUrl: optionalEnvironment("SOIL_DASHBOARD_URL"),
  };
}

async function authorizeCron(
  headers: Headers,
  expected: string,
): Promise<boolean> {
  const match = /^Bearer ([!-~]{32,512})$/.exec(
    headers.get("authorization") ?? "",
  );
  const [supplied, wanted] = await Promise.all([
    crypto.subtle.digest("SHA-256", encoder.encode(match?.[1] ?? "")),
    crypto.subtle.digest("SHA-256", encoder.encode(expected)),
  ]);
  return match !== null &&
    constantTimeEqual(new Uint8Array(supplied), new Uint8Array(wanted));
}

function describe(alert: AlertTransition, dashboardUrl: string | null): {
  subject: string;
  body: string;
} {
  const detail = alert.detail ?? {};
  const device = alert.device_id;
  const cleared = alert.transition === "cleared";

  let subject: string;
  let body: string;

  switch (alert.alert_kind) {
    case "dry_soil": {
      const moisture = detail.moisture ?? "unknown";
      const threshold = detail.threshold ?? "unknown";
      subject = cleared ? `Soil moisture recovered on ${device}` : `Soil is dry on ${device}`;
      body = cleared
        ? `Moisture is back above the ${threshold}% threshold.`
        : `Moisture has fallen to ${moisture}%, below the ${threshold}% threshold. Time to water.`;
      break;
    }
    case "sensor_offline": {
      const lastSeen = detail.lastSampledAt ?? "never";
      subject = cleared
        ? `Sensor ${device} is reporting again`
        : `Sensor ${device} has stopped reporting`;
      body = cleared
        ? "Telemetry has resumed."
        : `No reading has arrived since ${lastSeen}. Check the battery, the antenna, and the gateway's power.`;
      break;
    }
    case "battery_low": {
      const batteryMv = detail.batteryMv ?? "unknown";
      subject = cleared ? `Battery recovered on ${device}` : `Battery is low on ${device}`;
      body = cleared
        ? "Battery voltage is back above the configured threshold."
        : `Battery is at ${batteryMv} mV. Recharge or check the solar panel before the node goes silent.`;
      break;
    }
  }

  if (dashboardUrl !== null) body += `\n\n${dashboardUrl}`;
  return { subject, body };
}

async function fetchTransitions(
  config: RuntimeConfig,
): Promise<AlertTransition[]> {
  const url = new URL("/rest/v1/rpc/evaluate_alerts", config.supabaseUrl);
  const response = await fetch(url, {
    method: "POST",
    headers: {
      apikey: config.serviceRoleKey,
      Authorization: `Bearer ${config.serviceRoleKey}`,
      "Content-Type": "application/json",
    },
    body: "{}",
  });
  if (!response.ok) {
    throw new Error(`alert evaluation failed (${response.status})`);
  }
  return await response.json() as AlertTransition[];
}

async function deliverEmail(
  config: RuntimeConfig,
  subject: string,
  body: string,
): Promise<boolean> {
  if (!config.resendApiKey || !config.alertFrom || !config.alertTo) {
    return false;
  }
  const response = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${config.resendApiKey}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      from: config.alertFrom,
      to: [config.alertTo],
      subject,
      text: body,
    }),
  });
  if (!response.ok) {
    console.error("alert email failed", response.status);
    return false;
  }
  return true;
}

async function deliverWebhook(
  config: RuntimeConfig,
  alert: AlertTransition,
  subject: string,
  body: string,
): Promise<boolean> {
  if (!config.webhookUrl) return false;
  const response = await fetch(config.webhookUrl, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      deviceId: alert.device_id,
      kind: alert.alert_kind,
      transition: alert.transition,
      title: subject,
      message: body,
      detail: alert.detail,
    }),
  });
  if (!response.ok) {
    console.error("alert webhook failed", response.status);
    return false;
  }
  return true;
}

Deno.serve(async (request: Request): Promise<Response> => {
  let config: RuntimeConfig;
  try {
    config = loadConfig();
  } catch (error) {
    console.error(
      "alert-monitor configuration error",
      error instanceof Error ? error.message : "unknown",
    );
    return Response.json({ error: "service_unavailable" }, { status: 503 });
  }

  if (request.method !== "POST") {
    return Response.json({ error: "method_not_allowed" }, { status: 405 });
  }
  if (!await authorizeCron(request.headers, config.cronSecret)) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }

  try {
    const transitions = await fetchTransitions(config);
    let delivered = 0;
    for (const alert of transitions) {
      const { subject, body } = describe(alert, config.dashboardUrl);
      // Both channels are attempted; either one succeeding counts as
      // delivered so a misconfigured webhook cannot suppress email.
      const results = await Promise.all([
        deliverEmail(config, subject, body),
        deliverWebhook(config, alert, subject, body),
      ]);
      if (results.some(Boolean)) delivered += 1;
      else console.error("no alert channel accepted", subject);
    }
    return Response.json({
      evaluated: transitions.length,
      delivered,
    });
  } catch (error) {
    console.error(
      "alert-monitor internal error",
      error instanceof Error ? error.message : "unknown",
    );
    return Response.json({ error: "service_unavailable" }, { status: 503 });
  }
});
