/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Client for the vendor pen service, written from scratch so that no vendor code is
 * bundled or redistributed.
 *
 * Protocol details were recovered by decompiling the vendor SDK and then verified on a
 * Xiaomi Pad 8; see docs/XIAOMI-SERVICE-REVERSE.md and docs/KRITA-PEN-INTEGRATION.md.
 *
 * The service is bound, we register our own callback together with the package name, and
 * only then do we ask it to enable stylus posture (barrel rotation) and touch film (slide,
 * double tap). That order matters: without the package name the ROM logs
 * "The package name of client <pid> is null, so the system is not notified" and delivers
 * nothing.
 */
package org.krita.android;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;
import android.view.MotionEvent;

/**
 * Binds the vendor pencil engine service and enables the vendor event stream.
 *
 * Callers get the barrel rotation through {@link Listener#onStylusRotation}. Nothing here
 * is vendor specific beyond the protocol constants, all of which live in this file so a
 * ROM change is a one file fix.
 */
public class PenEngineClient implements IBinder.DeathRecipient {

    private static final String TAG = "krita.PenEngine";

    // --- Vendor protocol, recovered from the SDK -------------------------------------

    private static final String VENDOR_PACKAGE = "com.xiaomi.touchservice";
    private static final String VENDOR_SERVICE =
            "com.xiaomi.touchservice.pencilengine.PencilEngineManagerService";
    private static final String ENGINE_DESCRIPTOR =
            "com.xiaomi.touchservice.pencilengine.IPencilEngine";
    private static final String CALLBACK_DESCRIPTOR =
            "com.xiaomi.touchservice.pencilengine.IPencilEngineCallback";

    private static final int TX_REGISTER_WITH_PACKAGE = 4;  // a(IPencilEngineCallback, String)
    private static final int TX_FEATURE = 3;                // int a(int feature, int value)
    private static final int CALLBACK_EVENT = 1;            // a(int, int, int)

    private static final int FEATURE_STYLUS_POSTURE = 1;    // barrel rotation
    private static final int FEATURE_TOUCH_FILM = 2;        // slide and double tap

    /** Source the ROM uses for the injected posture events. */
    public static final int POSTURE_SOURCE = 0x01000010;
    /** Axis carrying the angle as a fraction of a full turn. */
    public static final int POSTURE_AXIS = 17;

    // -------------------------------------------------------------------------------

    public interface Listener {
        /** @param degrees barrel rotation in degrees, -180..180. */
        void onStylusRotation(int degrees);

        /** Vendor touch film callback, code values come from the ROM. */
        void onTouchFilm(int code);
    }

    private final Context mContext;
    private final Listener mListener;
    private final Handler mMain = new Handler(Looper.getMainLooper());

    private boolean mBound = false;
    private boolean mRegistered = false;
    private IBinder mService = null;

    public PenEngineClient(Context context, Listener listener)
    {
        mContext = context.getApplicationContext();
        mListener = listener;
    }

    /** Bind the service. Safe to call again after a disconnect. */
    public void bind()
    {
        if (mBound) {
            return;
        }
        try {
            Intent intent = new Intent();
            intent.setComponent(new ComponentName(VENDOR_PACKAGE, VENDOR_SERVICE));
            mBound = mContext.bindService(intent, mConnection, Context.BIND_AUTO_CREATE);
            Log.i(TAG, "bound=" + mBound);
        } catch (Throwable t) {
            // A device or ROM without the service is expected; stay silent and featureless.
            Log.i(TAG, "vendor service unavailable: " + t);
        }
    }

    public void unbind()
    {
        if (mBound) {
            try {
                mContext.unbindService(mConnection);
            } catch (Throwable ignored) {
            }
        }
        mBound = false;
        mRegistered = false;
        mService = null;
    }

    /**
     * Feed a generic motion event. Returns true when it was the vendor posture stream and
     * the angle was decoded.
     */
    public boolean handleMotionEvent(MotionEvent event)
    {
        if (event.getSource() != POSTURE_SOURCE
                || event.getAction() != MotionEvent.ACTION_MOVE) {
            return false;
        }
        final int degrees = (int) (event.getAxisValue(POSTURE_AXIS) * 360.0f);
        mMain.post(new Runnable() {
            @Override
            public void run() {
                if (mListener != null) {
                    mListener.onStylusRotation(degrees);
                }
            }
        });
        return true;
    }

    @Override
    public void binderDied()
    {
        Log.i(TAG, "vendor service died, reconnecting");
        mService = null;
        mRegistered = false;
        mBound = false;
        bind();
    }

    private final ServiceConnection mConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service)
        {
            mService = service;
            mRegistered = false;
            try {
                service.linkToDeath(PenEngineClient.this, 0);
            } catch (Throwable ignored) {
            }
            enableFeatures();
        }

        @Override
        public void onServiceDisconnected(ComponentName name)
        {
            mService = null;
            mRegistered = false;
        }
    };

    private void enableFeatures()
    {
        // Registration must carry the package name and must happen before enabling, or
        // the ROM refuses to notify the system and no stream is delivered.
        if (!registerWithPackage()) {
            return;
        }
        callFeature(FEATURE_STYLUS_POSTURE, 1);
        callFeature(FEATURE_TOUCH_FILM, 1);
    }

    private boolean registerWithPackage()
    {
        if (mService == null) {
            return false;
        }
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(ENGINE_DESCRIPTOR);
            data.writeStrongBinder(new Callback());
            data.writeString(mContext.getPackageName());
            final boolean ok = mService.transact(TX_REGISTER_WITH_PACKAGE, data, reply, 0);
            reply.readException();
            mRegistered = ok;
            Log.i(TAG, "registered=" + ok);
            return ok;
        } catch (Throwable t) {
            Log.i(TAG, "register failed: " + t);
            return false;
        } finally {
            data.recycle();
            reply.recycle();
        }
    }

    private void callFeature(int feature, int value)
    {
        if (mService == null) {
            return;
        }
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(ENGINE_DESCRIPTOR);
            data.writeInt(feature);
            data.writeInt(value);
            mService.transact(TX_FEATURE, data, reply, 0);
            reply.readException();
            Log.i(TAG, "feature(" + feature + "," + value + ") requested");
        } catch (Throwable t) {
            Log.i(TAG, "feature(" + feature + ") failed: " + t);
        } finally {
            data.recycle();
            reply.recycle();
        }
    }

    /** Our half of IPencilEngineCallback. */
    private final class Callback extends Binder {
        Callback() {
            attachInterface(null, CALLBACK_DESCRIPTOR);
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException
        {
            if (code != CALLBACK_EVENT) {
                return super.onTransact(code, data, reply, flags);
            }
            data.enforceInterface(CALLBACK_DESCRIPTOR);
            final int first = data.readInt();
            data.readInt();
            data.readInt();
            if (reply != null) {
                reply.writeNoException();
            }
            mMain.post(new Runnable() {
                @Override
                public void run() {
                    if (mListener != null) {
                        mListener.onTouchFilm(first);
                    }
                }
            });
            return true;
        }
    }
}
