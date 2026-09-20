import { describe, expect, it } from 'vitest';
import { markAllNotificationsRead, notifyApp, subscribeAppNotificationHistory } from './notifications';

describe('application notifications', () => {
  it('publishes a new unread notification to history', () => {
    let latest: ReturnType<typeof notifyApp>[] = [];
    const unsubscribe = subscribeAppNotificationHistory((history) => {
      latest = history;
    });

    const notification = notifyApp({ kind: 'success', message: 'Project opened.' });

    expect(latest[0]).toMatchObject({
      id: notification.id,
      kind: 'success',
      message: 'Project opened.',
      read: false,
    });
    unsubscribe();
  });

  it('marks the notification history as read when requested', () => {
    let latest: ReturnType<typeof notifyApp>[] = [];
    const unsubscribe = subscribeAppNotificationHistory((history) => {
      latest = history;
    });

    notifyApp({ kind: 'info', message: 'Canvas ready.' });
    markAllNotificationsRead();

    expect(latest.every((notification) => notification.read)).toBe(true);
    unsubscribe();
  });
});
