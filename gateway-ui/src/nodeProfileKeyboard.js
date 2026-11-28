export function handleProfileEditorKeyDown(event, busy) {
    if (event.key !== 'Enter' || event.shiftKey || event.nativeEvent?.isComposing || event.nativeEvent?.keyCode === 229) return;
    event.preventDefault();
    if (!busy && !event.repeat) event.currentTarget.form?.requestSubmit();
}
