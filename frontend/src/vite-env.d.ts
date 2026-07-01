/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Fallback host used when the frontend is served locally (localhost / 127.0.0.1). Defaults to 8.8.8.8. */
  readonly VITE_LOCAL_DEVICE_HOST?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
