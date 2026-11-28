export const NODE_CARD_MIN_WIDTH = 340;
export const NODE_CARD_GAP = 14;

// Use the original closed grid's column width. Opening a chart does not enter
// this calculation, so neither the selected card nor other cards are resized.
export function nodeCardWidth(containerWidth) {
    if (!Number.isFinite(containerWidth) || containerWidth <= 0) return NODE_CARD_MIN_WIDTH;
    const columns = Math.max(1, Math.floor((containerWidth+NODE_CARD_GAP)/(NODE_CARD_MIN_WIDTH+NODE_CARD_GAP)));
    return (containerWidth-NODE_CARD_GAP*(columns-1))/columns;
}
