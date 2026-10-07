package com.github.mw3ds.scenario;

import com.github.mw3ds.entity.PlayerEntity;
import com.github.mw3ds.entity.guards.GuardEntity;
import com.github.mw3ds.game.GameManager;
import com.github.mw3ds.item.Item;
import com.github.mw3ds.world.World;

import org.joml.Vector2f;

import java.util.List;
import java.util.ArrayList;

/**
 * The intro scenario where the player selects their class.
 * 
 * KEY BEHAVIOR: Stealing in this scenario should NOT result in:
 * - Jail (would break the intro sequence)
 * - Death/combat (would break the intro sequence)
 * 
 * The only consequence should be confiscation of the stolen item.
 */
public class IntroScenario extends Scenario {

    private boolean papersDelivered = false;
    private List<GuardEntity> guards = new ArrayList<>();

    public IntroScenario(World world, GameManager gameManager) {
        super(world, gameManager);
    }

    @Override
    public void onStart() {
        super.onStart();
        setupIntroRoom();
    }

    /**
     * Sets up the intro class-selection room.
     * Places guards in INTRO MODE so they only confiscate, not arrest.
     */
    private void setupIntroRoom() {
        // Create the class selection area
        createClassSelectionArea();

        // Place guards and set them to intro mode
        Vector2f guardPos1 = new Vector2f(2.0f, 3.0f);
        Vector2f guardPos2 = new Vector2f(8.0f, 3.0f);

        GuardEntity guard1 = new GuardEntity(getWorld(), guardPos1, this);
        GuardEntity guard2 = new GuardEntity(getWorld(), guardPos2, this);

        // CRITICAL: Set guards to intro mode
        // This prevents them from jailing or attacking the player
        guard1.setIntroMode(true);
        guard2.setIntroMode(true);

        guards.add(guard1);
        guards.add(guard2);

        getWorld().addEntity(guard1);
        getWorld().addEntity(guard2);

        // Place the release papers item
        Vector2f papersPos = new Vector2f(5.0f, 5.0f);
        Item papers = Item.getItem("release_papers");
        papers.setPosition(papersPos);
        getWorld().addItem(papers);
    }

    /**
     * Creates the class selection UI area.
     */
    private void createClassSelectionArea() {
        // Setup the visual area where classes are displayed
        // This is primarily visual/UI based
    }

    /**
     * Checks if the player has progressed through the intro.
     */
    public void checkProgress() {
        PlayerEntity player = getWorld().getPlayer();

        // Check if player has the release papers
        if (player.getInventory().hasItem("release_papers") && !papersDelivered) {
            papersDelivered = true;
            deliverPapers(player);
        }
    }

    /**
     * Handles the paper delivery event.
     * Grants the player their starting inventory.
     */
    private void deliverPapers(PlayerEntity player) {
        // Grant inventory items
        player.getInventory().addItem(Item.getItem("notebook"));
        player.getInventory().addItem(Item.getItem("id_card"));
        player.getInventory().addItem(Item.getItem("radio"));

        // Remove the papers from inventory (they've been "delivered")
        player.getInventory().removeItemByName("release_papers");

        // Trigger the transition to the next part of the intro
        getGameManager().scheduleTransition("class_selected");
    }

    /**
     * Gets the list of guards in this scenario.
     */
    public List<GuardEntity> getGuards() {
        return guards;
    }

    @Override
    public void onUpdate(float delta) {
        super.onUpdate(delta);
        checkProgress();

        // Update guards - they will handle stealing detection
        // but will only confiscate due to intro mode
        for (GuardEntity guard : guards) {
            guard.update(delta);
        }
    }

    @Override
    public void onComplete() {
        // Clean up intro-specific state
        guards.clear();
    }
}
