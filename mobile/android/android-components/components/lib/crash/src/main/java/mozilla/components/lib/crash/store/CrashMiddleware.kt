/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.lib.crash.store

import androidx.annotation.StringRes
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import mozilla.components.lib.crash.CrashReporter
import mozilla.components.lib.crash.R

/**
 * Represents the available options for crash reporting preferences.
 *
 * @property labelId The string resource label ID associated with the option.
 */
enum class CrashReportOption(@param:StringRes val labelId: Int) {
    Ask(R.string.crash_reporting_ask),
    Auto(R.string.crash_reporting_auto),
    Never(R.string.crash_reporting_never),
}

/**
 * An interface to store and retrieve a timestamp to defer submitting unsent crashes until.
 */
interface CrashReportCache {
    /**
     * Retrieves the previously stored cutoff date for crash reports.
     *
     * @return The cutoff date as a timestamp in milliseconds, or `null` if none has been set.
     */
    suspend fun getCutoffDate(): TimeInMillis?

    /**
     * Stores a cutoff date for crash reports.
     *
     * @param timeInMillis The cutoff date as a timestamp in milliseconds or `null`.
     */
    suspend fun setCutoffDate(timeInMillis: TimeInMillis?)

    /**
     * Gets the stored deferred timestamp.
     */
    suspend fun getDeferredUntil(): TimeInMillis?

    /**
     * Stores a deferred timestamp.
     */
    suspend fun setDeferredUntil(timeInMillis: TimeInMillis?)

    /**
     * Check to see if the user always wants to send crash reports.
     */
    suspend fun getAlwaysSend(): Boolean

    /**
     * Stores the users response to always send crashes.
     */
    suspend fun setAlwaysSend(alwaysSend: Boolean)

    /**
     * Records that the user does not want to see the remote settings crash pull
     * anymore
     */
    suspend fun setCrashPullNeverShowAgain(neverShowAgain: Boolean)

    /**
     * Gets the currently set crash report option ('Ask', 'Always' or 'Never')
     */
    suspend fun getReportOption(): CrashReportOption

    /**
     * Stores the currently set crash report option ('Ask', 'Always' or 'Never')
     */
    suspend fun setReportOption(option: CrashReportOption)
}

/**
 * Middleware for the crash reporter.
 *
 * @param cache stored values for getting/setting deferredUntil.
 * @param crashReporter instance of [CrashReporter] for checking for and sending unsent crashes.
 * @param currentTimeInMillis get the current time in milliseconds.
 * @param scope [CoroutineScope] to run suspended functions on.
 */
class CrashMiddleware(
    private val cache: CrashReportCache,
    private val crashReporter: CrashReporter,
    private val currentTimeInMillis: () -> TimeInMillis = { System.currentTimeMillis() },
    private val scope: CoroutineScope = CoroutineScope(Dispatchers.IO),
) {
    /**
     * Handle any middleware logic before an action reaches the [crashReducer].
     *
     * @param middlewareContext accessors for the current [CrashState] and dispatcher from the store.
     * @param next The next middleware in the chain.
     * @param action The current [CrashAction] to process in the middleware.
     */
    @Suppress("CyclomaticComplexMethod")
    fun invoke(
        middlewareContext: Pair<() -> CrashState, (CrashAction) -> Unit>,
        next: (CrashAction) -> Unit,
        action: CrashAction,
    ) {
        val (getState, dispatch) = middlewareContext

        next(action)

        when (action) {
            is CrashAction.Initialize -> scope.launch {
                val nextAction = if (cache.getAlwaysSend()) {
                    CrashAction.CheckForCrashes
                } else {
                    CrashAction.CheckDeferred
                }

                dispatch(nextAction)
            }
            is CrashAction.CheckDeferred -> scope.launch {
                val nextAction = cache.getDeferredUntil()?.let {
                    CrashAction.RestoreDeferred(now = currentTimeInMillis(), until = it)
                } ?: CrashAction.CheckForCrashes

                dispatch(nextAction)
            }
            is CrashAction.RestoreDeferred -> {
                if (getState() is CrashState.Ready) {
                    scope.launch {
                        cache.setDeferredUntil(null)
                        dispatch(CrashAction.CheckForCrashes)
                    }
                }
            }
            is CrashAction.CheckForCrashes -> scope.launch {
                dispatch(CrashAction.FinishCheckingForCrashes(crashReporter.hasUnsentCrashReportsSince(cutoffDate())))
            }
            is CrashAction.FinishCheckingForCrashes -> scope.launch {
                if (!action.hasUnsentCrashes) { return@launch }
                if (cache.getAlwaysSend()) {
                    sendUnsentCrashReports()
                } else {
                    dispatch(CrashAction.ShowPrompt)
                }
            }
            CrashAction.CancelTapped -> dispatch(CrashAction.Defer(now = currentTimeInMillis()))
            CrashAction.CancelForEverTapped -> scope.launch {
                cache.setCrashPullNeverShowAgain(true)
            }
            is CrashAction.Defer -> scope.launch {
                val state = getState()
                if (state is CrashState.Deferred) {
                    cache.setDeferredUntil(state.until)
                }
            }
            is CrashAction.ReportTapped -> scope.launch {
                if (action.crashIDs != null && action.crashIDs.size > 0) {
                    sendCrashReports(action.crashIDs)
                } else {
                    if (action.automaticallySendChecked) {
                        cache.setAlwaysSend(true)
                    }
                    sendUnsentCrashReports()
                }
            }
            is CrashAction.PullCrashes -> {} // noop
            CrashAction.ShowPrompt -> {} // noop
        }
    }

    private suspend fun cutoffDate(): TimeInMillis {
        return cache.getCutoffDate() ?: currentTimeInMillis().also {
            cache.setCutoffDate(it)
        }
    }

    private suspend fun sendUnsentCrashReports() {
        crashReporter.unsentCrashReportsSince(cutoffDate()).forEach {
            crashReporter.submitReport(it)
        }
    }

    private suspend fun sendCrashReports(crashIDs: Array<String>) {
        crashReporter.findCrashReports(crashIDs).forEach {
            crashReporter.submitReport(it)
        }
    }
}
