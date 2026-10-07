package com.github.mw3ds.intro;

import com.github.mw3ds.entity.PlayerEntity;
import com.github.mw3ds.entity.Entity;
import com.github.mw3ds.entity guards.GuardEntity;
import com.github.mw3ds.game.GameManager;
import com.github.mw3ds.scene.Scene;
import com.github.mw3ds.world.World;
import com.github.mw3ds.item.Item;
import com.github.mw3ds.inventory.Inventory;

import org.joml.Vector2f;

/**
 * Intro scene for the class selection room.
 * In the original MW3, stealing in this room should only result in the guard
 * taking the stolen item back, NOT jailing or killing the player,
 * as jailing would break the intro sequence.
 */
public class IntroScene extends Scene {

    private boolean papersCollected = false;
    private boolean canSteal = true;
    private PlayerEntity player;

    public IntroScene(World world, GameManager gameManager) {
        super(world, gameManager);
    }

    @Override
    public void init() {
        // Set up the class selection room
        setupRoom();
    }

    @Override
    public void update(float delta) {
        super.update(delta);

        if (player == null) {
            player = getWorld().getPlayer();
        }

        // Check if player collected release papers
        checkPapersCollection();

        // Handle stealing behavior - special case for intro
        checkStealing();
    }

    /**
     * Check if the player has collected the release papers.
     * Once collected, the stealing penalty is reduced to just
     * returning the stolen item (no jail/kick).
     */
    private void checkPapersCollection() {
        if (papersCollected) return;

        // Papers are in a specific location in the room
        Vector2f papersPos = new Vector2f(0.5f, 0.3f); // Adjust based on actual layout
        float dist = player.getPosition().dst(papersPos);

        if (dist < 1.5f && player.hasKey("release_papers")) {
            papersCollected = true;
            // Grant inventory as per original game
            grantIntroInventory();
        }
    }

    /**
     * Handles stealing detection and response.
     * In the intro scene, stealing only results in the guard
     * taking the item back - no jail or death.
     */
    private void checkStealing() {
        if (!canSteal || !papersCollected) return;

        Inventory playerInv = player.getInventory();
        for (Item item : playerInv.getItems()) {
            if (item.isStolen()) {
                // Guard takes the stolen item back only
                // No jail, no kill, just confiscation
                GuardEntity nearbyGuard = findNearbyGuard();
                if (nearbyGuard != null) {
                    nearbyGuard.confiscateItem(player, item);
                } else {
                    // If no guard nearby, just remove the item
                    playerInv.removeItem(item);
                }
                // Disable further stealing to prevent abuse
                canSteal = false;
                break;
            }
        }
    }

    private GuardEntity findNearbyGuard() {
        for (Entity entity : getWorld().getEntities()) {
            if (entity instanceof GuardEntity) {
                float dist = entity.getPosition().dst(player.getPosition());
                if (dist < 3.0f) {
                    return (GuardEntity) entity;
                }
            }
        }
        return null;
    }

    private void grantIntroInventory() {
        // Grant the standard intro inventory items
        // This mirrors the original game's behavior
        Inventory inv = player.getInventory();
        // Add standard starting items
        inv.addItem(Item.getItem("notebook"));
        inv.addItem(Item.getItem("id_card"));
    }

    @Override
    public void render() {
        super.render();
        // Render class selection UI
        renderClassSelectionUI();
    }

    private void renderClassSelectionUI() {
        // Draw the class selection interface
        // This is a simplified version - adjust for actual rendering system
    }

    private void setupRoom() {
        // Setup the intro room geometry and props
        // Place the release papers object
        // Place guard entities
    }

    /**
     * Called when the player progresses past the intro.
     */
    public void completeIntro() {
        // Transition to the next scene
        getGameManager().loadNextScene();
    }
}
