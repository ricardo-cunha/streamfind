export function StartupFailureScreen({ error }: { error: unknown }) {
  const message = error instanceof Error ? error.message : 'The frontend could not start.';
  return (
    <main className="sf-startup-failure" role="alert">
      <h1>StreamFind could not start</h1>
      <p>{message}</p>
      <button type="button" onClick={() => window.location.reload()}>
        Retry
      </button>
    </main>
  );
}
