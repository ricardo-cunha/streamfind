import { describe, expect, it } from 'vitest';
import { notifyApp, subscribeAppNotificationHistory } from './notifications';

describe('application notifications', () => {
  it('publishes a new notification to the history log', () => {
    let latest: ReturnType<typeof notifyApp>[] = [];
    const unsubscribe = subscribeAppNotificationHistory((history) => {
      latest = history;
    });

    const notification = notifyApp({ kind: 'success', message: 'Project opened.' });

    expect(latest[0]).toMatchObject({
      id: notification.id,
      kind: 'success',
      message: 'Project opened.',
    });
    unsubscribe();
  });
});
