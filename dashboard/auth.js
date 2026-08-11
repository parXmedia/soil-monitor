export class PasskeyAuth {
  constructor(client) {
    this.client = client;
  }

  async getSession() {
    const { data, error } = await this.client.auth.getSession();
    if (error) throw error;
    return data.session;
  }

  onAuthStateChange(callback) {
    return this.client.auth.onAuthStateChange(callback);
  }

  async signInWithPasskey() {
    const { data, error } = await this.client.auth.signInWithPasskey();
    if (error) throw error;
    return data.session;
  }

  async sendEmailLink(email, redirectTo) {
    const { error } = await this.client.auth.signInWithOtp({
      email,
      options: { emailRedirectTo: redirectTo, shouldCreateUser: false }
    });
    if (error) throw error;
  }

  async registerPasskey() {
    const { data, error } = await this.client.auth.registerPasskey();
    if (error) throw error;
    return data;
  }

  async listPasskeys() {
    const { data, error } = await this.client.auth.passkey.list();
    if (error) throw error;
    return Array.isArray(data) ? data : [];
  }

  async signOut() {
    const { error } = await this.client.auth.signOut();
    if (error) throw error;
  }
}

export function createPasskeyAuth(config, scope = globalThis) {
  const createClient = scope.supabase?.createClient;
  if (typeof createClient !== "function") {
    throw new Error("The local Supabase authentication client did not load.");
  }
  const client = createClient(config.supabaseUrl, config.publishableKey, {
    auth: {
      autoRefreshToken: true,
      detectSessionInUrl: true,
      persistSession: true,
      experimental: { passkey: true }
    }
  });
  return new PasskeyAuth(client);
}
